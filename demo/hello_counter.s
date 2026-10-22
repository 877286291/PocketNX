# =============================================================================
# hello_counter.s — PocketNX M1 demo homebrew ("hello counter")
#
# A minimal, self-contained AArch64 program for the direct-execution runtime.
# No libc, no external symbols, no relocations: the whole module is one
# __TEXT,__text section. The 5x7 font table lives at the tail of the same
# section and is reached with ADR (same-section, +/-1 MB), so the object
# carries no relocation entries at all.
#
# Entry (main, the first instruction of __text) receives
#   x0 = &GuestFrameInfo  (see Core/src/gpu/Framebuffer.h)
#     +0   u32 width
#     +4   u32 height
#     +8   u32 stride
#     +12  u32 format
#     +16  u32 capacity
#     +20  u32 max_frames   (0 = run forever)
#     +24  u64 pixels       (RGBA8888 buffer)
#     +32  u64 counter      (frame counter, published each frame)
#
# Each frame:
#   1. clear the screen to a dark background     0xFF101828
#   2. draw "HELLO" (5x7 font, 2x scale) at (24, 24)
#   3. draw the last two counter digits at (100, 24) and (112, 24)
#   4. draw a sweeping bar at y = 200
#   5. svc #0x0B SleepThread(16 ms)
# After max_frames frames (when non-zero) the guest returns to the host.
#
# Register discipline: the guest never touches the stack (x31 = SP is never
# used as a general register; x29/x30 are untouched). x8..x12, x15, x17, x19,
# x20, x21 hold persistent state across the whole loop; x1..x7, x13, x14,
# x16, x18, x22..x28 are scratch. The SVC trap captures and restores the full
# register file, so persistent state survives SleepThread. x21 keeps
# &GuestFrameInfo because x0 is clobbered by SVC return values.
#
# W/X aliasing: a 32-bit write to Wn also writes Xn's low 32 bits. So a
# counter never shares a register with a 64-bit pointer — row/col counters
# use w2/w3, never w14/w27 (which alias the x14/x27 pointer registers).
# =============================================================================

	.text
	.section __TEXT,__text
	.p2align 2
	.globl main

main:
	// ---- load frame info -------------------------------------------------
	ldr	x8,  [x0, #24]		// x8  = pixels
	ldr	w9,  [x0, #0]		// w9  = width
	ldr	w10, [x0, #4]		// w10 = height
	ldr	w11, [x0, #8]		// w11 = stride
	ldr	w12, [x0, #20]		// w12 = max_frames
	mov	x21, x0			// x21 = &info (persistent; x0 is clobbered by SVCs)
	mov	w15, #0			// w15 = frame counter
	mov	w17, #0			// w17 = bar fill (px)
	adr	x19, .Lfont		// x19 = font table (tail of .text)
	adr	x20, .Lword		// x20 = "HELLO" glyph-index blob

.Lframe:
	// ---- 1. clear to background ------------------------------------------
	mov	x13, x8			// row pointer
	mov	w2, w10			// rows left (w2: row count — x14 is the row ptr)
	mov	w16, #0x1828
	movk	w16, #0xFF10, lsl #16	// 0xFF101828 (dark)
.Lclear_rows:
	lsr	w18, w11, #2		// words per row (reset every row)
	mov	x14, x13			// row start
.Lclear_cols:
	str	w16, [x14], #4
	sub	w18, w18, #1
	cbnz	w18, .Lclear_cols
	add	x13, x13, x11		// next row
	sub	w2, w2, #1
	cbnz	w2, .Lclear_rows

	// ---- 2. "HELLO" at (24, 24), scale 2 ----------------------------------
	// Glyph block contract (used for HELLO and both digits):
	//   in : w22 = x (px), x24 = &font[idx] (idx*35 bytes in), w16 = lit color
	//   out: clobbers w3, w18, w25, w28, w29, x13, x26, x27
	//   (w3 = col count, kept apart from x27 which holds the px offset)
	mov	w16, #0x3333
	movk	w16, #0xFF33, lsl #16	// 0xFF33FF33 (bright)
	mov	w14, #5			// 5 chars
	mov	w22, #24			// first char x
	mov	x1, x20			// blob pointer (scratch; x21 = &info)
.Lchars:
	ldrb	w23, [x1], #1		// glyph index
	lsl	w24, w23, #3
	sub	w24, w24, w23		// w24 = idx * 7 (glyphs are 7 bytes: 7 rows x 1 byte)
	add	x24, x19, x24		// x24 = &font[idx]
	mov	w25, #0			// row (0..6)
.Lglyph_rows:
	// row pointer = pixels + (24 + 2*row) * stride
	add	w18, w25, w25
	add	w18, w18, #24
	mul	w18, w18, w11
	add	x13, x8, x18
	ldrb	w28, [x24, x25]		// row byte (5 bits, MSB = leftmost)
	mov	w3,  #5			// cols (0..4); w3 (not w27: x27 holds the px offset)
	lsl	x27, x22, #2		// px offset from row start = x * 4
.Lglyph_cols:
	lsr	w29, w28, #4
	and	w29, w29, #1
	cbnz	w29, .Ldraw_px
	b	.Lcol_off
.Ldraw_px:
	add	x26, x13, x27
	str	w16, [x26]
	str	w16, [x26, #4]
	add	x26, x26, x11
	str	w16, [x26]
	str	w16, [x26, #4]
	b	.Lcol_off
.Lcol_off:
	lsl	w28, w28, #1
	add	x27, x27, #8		// +2 px * 4
	sub	w3, w3, #1
	cbnz	w3, .Lglyph_cols
	add	w25, w25, #1
	cmp	w25, #7
	b.lt	.Lglyph_rows
	add	w22, w22, #12		// next char (10 px glyph + 2 px gap)
	sub	w14, w14, #1
	cbnz	w14, .Lchars

	// ---- 3. counter digits at (100, 24) and (112, 24) ---------------------
	mov	w27, #10
	udiv	w25, w15, w27		// w25 = w15 / 10
	mul	w28, w25, w27
	sub	w28, w15, w28		// w28 = ones digit
	udiv	w29, w25, w27		// w29 = (w15/10) / 10
	mul	w29, w29, w27
	sub	w25, w25, w29		// w25 = (w15/10) % 10 = tens digit
	mov	w14, w28			// save ones (glyph block clobbers w28)
	mov	w22, #100			// tens digit x
	lsl	w24, w25, #3
	sub	w24, w24, w25		// w24 = idx * 7
	add	x24, x19, x24		// x24 = &font[tens]
	mov	w25, #0			// row (0..6)
.Ltens_rows:
	add	w18, w25, w25
	add	w18, w18, #24
	mul	w18, w18, w11
	add	x13, x8, x18
	ldrb	w28, [x24, x25]
	mov	w3, #5
	lsl	x27, x22, #2
.Ltens_cols:
	lsr	w29, w28, #4
	and	w29, w29, #1
	cbnz	w29, .Ldraw_px2
	b	.Lcol_off2
.Ldraw_px2:
	add	x26, x13, x27
	str	w16, [x26]
	str	w16, [x26, #4]
	add	x26, x26, x11
	str	w16, [x26]
	str	w16, [x26, #4]
	b	.Lcol_off2
.Lcol_off2:
	lsl	w28, w28, #1
	add	x27, x27, #8
	sub	w3, w3, #1
	cbnz	w3, .Ltens_cols
	add	w25, w25, #1
	cmp	w25, #7
	b.lt	.Ltens_rows
	mov	w22, #112			// ones digit x
	lsl	w24, w14, #3
	sub	w24, w24, w14		// w24 = idx * 7
	add	x24, x19, x24		// x24 = &font[ones]
	mov	w25, #0
.Lones_rows:
	add	w18, w25, w25
	add	w18, w18, #24
	mul	w18, w18, w11
	add	x13, x8, x18
	ldrb	w28, [x24, x25]
	mov	w3, #5
	lsl	x27, x22, #2
.Lones_cols:
	lsr	w29, w28, #4
	and	w29, w29, #1
	cbnz	w29, .Ldraw_px3
	b	.Lcol_off3
.Ldraw_px3:
	add	x26, x13, x27
	str	w16, [x26]
	str	w16, [x26, #4]
	add	x26, x26, x11
	str	w16, [x26]
	str	w16, [x26, #4]
	b	.Lcol_off3
.Lcol_off3:
	lsl	w28, w28, #1
	add	x27, x27, #8
	sub	w3, w3, #1
	cbnz	w3, .Lones_cols
	add	w25, w25, #1
	cmp	w25, #7
	b.lt	.Lones_rows

	// ---- 4. sweeping bar at y = 200 (skipped on tiny framebuffers) --------
	cmp	w10, #208
	b.le	.Lskipbar
	mov	w16, #0xCC00
	movk	w16, #0xFFFF, lsl #16	// 0xFFFFCC00 (bright warm)
	mov	x13, x8
	add	x13, x13, x11, lsl #7	// +128 rows
	add	x13, x13, x11, lsl #6	// +64 rows  (total +200 * stride)
	add	x13, x13, x11, lsl #3	// +8 rows
	mov	w2, #8			// bar height (rows) (w2: row count — x14 is the row ptr)
.Lbar_rows:
	mov	x14, x13
	mov	w18, w17			// px left
.Lbar_cols:
	cbz	w18, .Lbar_row_done	// no px left on this row (fill may be 0)
	str	w16, [x14], #4
	sub	w18, w18, #1
	b	.Lbar_cols
.Lbar_row_done:
	add	x13, x13, x11
	sub	w2, w2, #1
	cbnz	w2, .Lbar_rows
	lsr	w24, w9, #4		// step = width / 16
	add	w17, w17, w24
	cmp	w17, w9
	b.lt	.Lbarwrap
	sub	w17, w17, w9	// wrap at full width
.Lbarwrap:
.Lskipbar:

	// ---- 5. advance counter, stop check, sleep ----------------------------
	add	w15, w15, #1
	str	x15, [x21, #32]		// publish frame counter
	cbz	w12, .Lnosleep
	cmp	w15, w12
	b.ge	.Ldone
.Lnosleep:
	mov	x0, #0x2400
	movk	x0, #0xF4, lsl #16	// 16,000,000 ns
	svc	#0xB			// SleepThread
	b	.Lframe

.Ldone:
	ret

	// =========================================================================
	// Data (reached via ADR, so it stays in __text: no cross-section relocs)
	// =========================================================================
	.p2align 2
.Lfont:			// 14 glyphs x 7 bytes (5x7, MSB = leftmost)
	.byte 0x0E, 0x11, 0x11, 0x15, 0x19, 0x11, 0x0E	// '0'
	.byte 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E	// '1'
	.byte 0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F	// '2'
	.byte 0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E	// '3'
	.byte 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02	// '4'
	.byte 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E	// '5'
	.byte 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E	// '6'
	.byte 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08	// '7'
	.byte 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E	// '8'
	.byte 0x0E, 0x11, 0x11, 0x0E, 0x01, 0x02, 0x0C	// '9'
	.byte 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11	// 'H' (idx 10)
	.byte 0x1F, 0x10, 0x1E, 0x10, 0x1F, 0x10, 0x1F	// 'E' (idx 11)
	.byte 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F	// 'L' (idx 12)
	.byte 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E	// 'O' (idx 13)
.Lword:			// glyph indices for "HELLO"
	.byte 12, 11, 12, 12, 13
