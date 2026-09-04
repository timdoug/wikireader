/*-
 * Copyright (c) 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Chris Torek.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
#include <string.h>

/*
 * sizeof(word) MUST BE A POWER OF TWO
 * SO THAT wmask BELOW IS ALL ONES
 */
typedef	unsigned int word;	/* "word" used for optimal copy speed */
typedef	unsigned long uptr;	/* pointer bits for alignment arithmetic */

#define	wsize	sizeof(word)
#define	wmask	(wsize - 1)

/*
 * Copy a block of memory, handling overlap.
 * This is the routine that actually implements
 * (the portable versions of) bcopy, memcpy, and memmove.
 *
 * The C33 traps every unaligned word access, so the original byte loop for
 * operands with differing alignment cost about five instructions per byte.
 * Forward copies of that kind now align the destination and assemble each
 * destination word from two aligned source words instead.  The aligned
 * source reads may touch up to three bytes on either side of the requested
 * range, but never outside the aligned words that contain requested bytes.
 */
#ifdef MEMCOPY
void *
memcpy(dst0, src0, length)
#else
#ifdef MEMMOVE
void *
memmove(dst0, src0, length)
#else
void
bcopy(src0, dst0, length)
#endif
#endif
void *dst0;
const void *src0;
register size_t length;
{
    register char *dst = dst0;
    register const char *src = src0;
    register size_t t;

    if (length == 0 || dst == src)		/* nothing to do */
        goto done;

    /*
     * Macros: loop-t-times; and loop-t-times, t>0
     */
#define	TLOOP(s) if (t) TLOOP1(s)
#define	TLOOP1(s) do { s; } while (--t)

    if ((uptr)dst < (uptr)src)
    {
        /*
         * Copy forward.
         */
        t = (uptr)src;	/* only need low bits */
        if ((t | (uptr)dst) & wmask)
        {
            if (((t ^ (uptr)dst) & wmask) && length >= 2 * wsize + wmask)
            {
                /*
                 * Alignments differ.  Align the destination, then
                 * shift pairs of aligned source words into place.
                 */
                unsigned int shift;
                const word *wp;
                word cur;

                t = (0 - (uptr)dst) & wmask;
                length -= t;
                TLOOP(*dst++ = *src++);
                shift = ((uptr)src & wmask) * 8;
                wp = (const word *)((uptr)src & ~(uptr)wmask);
                cur = *wp++;
                t = length / wsize;
                src += t * wsize;
                TLOOP1(word next = *wp++;
                       *(word *)dst = (cur >> shift) |
                                      (next << (8 * wsize - shift));
                       cur = next;
                       dst += wsize);
                t = length & wmask;
                TLOOP(*dst++ = *src++);
                goto done;
            }
            /*
             * Try to align operands.  This cannot be done
             * unless the low bits match.
             */
            if ((t ^ (uptr)dst) & wmask || length < wsize)
                t = length;
            else
                t = wsize - (t & wmask);
            length -= t;
            TLOOP1(*dst++ = *src++);
        }
        /*
         * Copy whole words, then mop up any trailing bytes.
         */
        t = length / wsize;
        TLOOP(*(word *)dst = *(const word *)src; src += wsize; dst += wsize);
        t = length & wmask;
        TLOOP(*dst++ = *src++);
    }
    else
    {
        /*
         * Copy backwards.  Otherwise essentially the same.
         * Alignment works as before, except that it takes
         * (t&wmask) bytes to align, not wsize-(t&wmask).
         */
        src += length;
        dst += length;
        t = (uptr)src;
        if ((t | (uptr)dst) & wmask)
        {
            if ((t ^ (uptr)dst) & wmask || length <= wsize)
                t = length;
            else
                t &= wmask;
            length -= t;
            TLOOP1(*--dst = *--src);
        }
        t = length / wsize;
        TLOOP(src -= wsize; dst -= wsize; *(word *)dst = *(const word *)src);
        t = length & wmask;
        TLOOP(*--dst = *--src);
    }
done:
#if defined(MEMCOPY) || defined(MEMMOVE)
    return (dst0);
#else
    return;
#endif
}
