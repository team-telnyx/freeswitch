/*
 * Copyright (c) 2016, Athonet (www.athonet.com)
 * Dragos Oancea  <dragos.oancea@athonet.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of the original author; nor the names of any contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#ifndef __AMRWB_BE_H__
#include "bitshift.h"
#include "amrwb_be.h"

/* Bandwidth Efficient AMR-WB */
/* https://tools.ietf.org/html/rfc4867#page-17 */

/* this works the same as in AMR NB*/
extern switch_bool_t switch_amrwb_pack_be(unsigned char *shift_buf, int n)
{
	uint8_t save_toc, ft;

	save_toc = shift_buf[1];

	/* we must convert OA TOC -> BE TOC */
	/* OA TOC
	0 1 2 3 4 5 6 7
	+-+-+-+-+-+-+-+-+
	|F|  FT   |Q|P1|P2|
	+-+-+-+-+-+-+-+-+
	F (1 bit): see definition in Section 4.3.2.

	FT (4 bits, unsigned integer): see definition in Section 4.3.2.

	Q (1 bit): see definition in Section 4.3.2.

	P bits: padding bits, MUST be set to zero, and MUST be ignored on reception.
	*/

	/* BE TOC:
	 0 1 2 3 4 5
	 +-+-+-+-+-+-+
	|F|  FT   |Q|
	+-+-+-+-+-+-+
	F = 0 , FT = XXXX , Q from the frame's ToC
	eg: Frame Types (FT): 3GPP TS 26.201, table 1a
	*/

	ft = save_toc >> 3 ; /* drop Q, P1, P2  */
	ft &= ~(1 << 4); /* clear F: this encoder emits exactly one frame */

	/* we only encode one frame, so bit 0 of TOC will be 0 */
	shift_buf[0] |= (ft >> 1); /* first 3 bits of FT */

	amrwb_array_lshift(6, shift_buf+1, n);
	/* Q from the frame's own ToC */
	if (save_toc & (1 << 2)) {
		shift_buf[1] |= 1 << 6;
	} else {
		shift_buf[1] &= ~(1 << 6);
	}
	/* the shift moved P1 to bit 7 and P2 to bit 6: last bit of FT in P1's place (Q is in P2's) */
	if (( ft >> 0 ) & 1) {
		shift_buf[1] |= 1 << 7;
	} else {
		shift_buf[1] &= ~(1 << 7);
	}

	return SWITCH_TRUE;
}

#endif

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
