;;; trailer.s
;;;
;;; Copyright (c) 2009 Openmoko Inc.
;;;
;;; Authors   Christopher Hall <hsw@openmoko.com>
;;;
;;; Redistribution and use in source and binary forms, with or without
;;; modification, are permitted provided that the following conditions are
;;; met:
;;;
;;;  1. Redistributions of source code must retain the above copyright
;;;     notice, this list of conditions and the following disclaimer.
;;;
;;;  2. Redistributions in binary form must reproduce the above copyright
;;;     notice, this list of conditions and the following disclaimer in
;;;     the documentation and/or other materials provided with the
;;;     distribution.
;;;
;;; THIS SOFTWARE IS PROVIDED BY THE CONTRIBUTORS ``AS IS'' AND ANY
;;; EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
;;; IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
;;; PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE CONTRIBUTORS BE LIABLE
;;; FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
;;; CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
;;; SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
;;; BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
;;; WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
;;; OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
;;; IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


;;; macro for regs.inc
        .macro  REGDEF, address, bits, name
R\()\bits\()_\name = \address
	CREATE	c33_dict "\name" \name 0
	.long	\address, \bits
        .endm

;;; A register bit is both a Forth constant and an assembler symbol, and
;;; they cannot share a name: the dictionary entry makes \name a label in
;;; .forth_dict, so the "\name = \value" below was redefining it.  Older
;;; assemblers let that pass; this one rejects it, and then rejects every
;;; later bit written as an OR of two of them, because the operands were
;;; section-relative rather than absolute.  Give the dictionary entry its
;;; own label and leave the bare name to the assembler.

        .macro  REGBIT, name, value
	CONSTANT c33_dict "\name" c33_bit_\name 0
        .long   \value
\name = \value
        .endm

        .macro  VECTOR, name, value
Vector_\()\name = \value
        .endm

        .include "c33regs.inc"


;;; finish off the dictionary
        .section .forth_dict
        .balign 4

dictionary_end:

;;; On the metal the dictionary grows into whatever SDRAM is left after the
;;; image.  Inside an operating system there is no "after": the next section
;;; belongs to something else.  Reserve the space the dictionary is allowed
;;; to take, as a section the loader does not have to carry, and let the
;;; linker script put it immediately after this one.

        .ifdef  NUTTX_FORTH
        .section .forth_heap, "aw", @nobits
        .balign 4
        .global forth_heap_start
forth_heap_start:
        .space  NUTTX_FORTH_DICT_SIZE
        .global forth_heap_end
forth_heap_end:
        .section .forth_dict
        .endif

root_last_name  = __root_dict_last_name         ; should be the final name
forth_last_name = __forth_dict_last_name        ; should be the final name
c33_last_name   = __c33_dict_last_name          ; should be the final name
