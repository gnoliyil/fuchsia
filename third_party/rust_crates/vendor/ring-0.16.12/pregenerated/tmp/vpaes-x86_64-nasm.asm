; This file is generated from a similarly-named Perl script in the BoringSSL
; source tree. Do not edit by hand.

default	rel
%define XMMWORD
%define YMMWORD
%define ZMMWORD
section	.text code align=64


















ALIGN	16
_vpaes_encrypt_core:

	mov	r9,rdx
	mov	r11,16
	mov	eax,DWORD[240+rdx]
	movdqa	xmm1,xmm9
	movdqa	xmm2,XMMWORD[$L$k_ipt]
	pandn	xmm1,xmm0
	movdqu	xmm5,XMMWORD[r9]
	psrld	xmm1,4
	pand	xmm0,xmm9
DB	102,15,56,0,208
	movdqa	xmm0,XMMWORD[(($L$k_ipt+16))]
DB	102,15,56,0,193
	pxor	xmm2,xmm5
	add	r9,16
	pxor	xmm0,xmm2
	lea	r10,[$L$k_mc_backward]
	jmp	NEAR $L$enc_entry

ALIGN	16
$L$enc_loop:

	movdqa	xmm4,xmm13
	movdqa	xmm0,xmm12
DB	102,15,56,0,226
DB	102,15,56,0,195
	pxor	xmm4,xmm5
	movdqa	xmm5,xmm15
	pxor	xmm0,xmm4
	movdqa	xmm1,XMMWORD[((-64))+r10*1+r11]
DB	102,15,56,0,234
	movdqa	xmm4,XMMWORD[r10*1+r11]
	movdqa	xmm2,xmm14
DB	102,15,56,0,211
	movdqa	xmm3,xmm0
	pxor	xmm2,xmm5
DB	102,15,56,0,193
	add	r9,16
	pxor	xmm0,xmm2
DB	102,15,56,0,220
	add	r11,16
	pxor	xmm3,xmm0
DB	102,15,56,0,193
	and	r11,0x30
	sub	rax,1
	pxor	xmm0,xmm3

$L$enc_entry:

	movdqa	xmm1,xmm9
	movdqa	xmm5,xmm11
	pandn	xmm1,xmm0
	psrld	xmm1,4
	pand	xmm0,xmm9
DB	102,15,56,0,232
	movdqa	xmm3,xmm10
	pxor	xmm0,xmm1
DB	102,15,56,0,217
	movdqa	xmm4,xmm10
	pxor	xmm3,xmm5
DB	102,15,56,0,224
	movdqa	xmm2,xmm10
	pxor	xmm4,xmm5
DB	102,15,56,0,211
	movdqa	xmm3,xmm10
	pxor	xmm2,xmm0
DB	102,15,56,0,220
	movdqu	xmm5,XMMWORD[r9]
	pxor	xmm3,xmm1
	jnz	NEAR $L$enc_loop


	movdqa	xmm4,XMMWORD[((-96))+r10]
	movdqa	xmm0,XMMWORD[((-80))+r10]
DB	102,15,56,0,226
	pxor	xmm4,xmm5
DB	102,15,56,0,195
	movdqa	xmm1,XMMWORD[64+r10*1+r11]
	pxor	xmm0,xmm4
DB	102,15,56,0,193
	DB	0F3h,0C3h		;repret









ALIGN	16
_vpaes_schedule_core:






	call	_vpaes_preheat
	movdqa	xmm8,XMMWORD[$L$k_rcon]
	movdqu	xmm0,XMMWORD[rdi]


	movdqa	xmm3,xmm0
	lea	r11,[$L$k_ipt]
	call	_vpaes_schedule_transform
	movdqa	xmm7,xmm0

	lea	r10,[$L$k_sr]


	movdqu	XMMWORD[rdx],xmm0

$L$schedule_go:
	cmp	esi,192
	ja	NEAR $L$schedule_256











$L$schedule_128:
	mov	esi,10

$L$oop_schedule_128:
	call	_vpaes_schedule_round
	dec	rsi
	jz	NEAR $L$schedule_mangle_last
	call	_vpaes_schedule_mangle
	jmp	NEAR $L$oop_schedule_128











ALIGN	16
$L$schedule_256:
	movdqu	xmm0,XMMWORD[16+rdi]
	call	_vpaes_schedule_transform
	mov	esi,7

$L$oop_schedule_256:
	call	_vpaes_schedule_mangle
	movdqa	xmm6,xmm0


	call	_vpaes_schedule_round
	dec	rsi
	jz	NEAR $L$schedule_mangle_last
	call	_vpaes_schedule_mangle


	pshufd	xmm0,xmm0,0xFF
	movdqa	xmm5,xmm7
	movdqa	xmm7,xmm6
	call	_vpaes_schedule_low_round
	movdqa	xmm7,xmm5

	jmp	NEAR $L$oop_schedule_256












ALIGN	16
$L$schedule_mangle_last:

	lea	r11,[$L$k_deskew]


	movdqa	xmm1,XMMWORD[r10*1+r8]
DB	102,15,56,0,193
	lea	r11,[$L$k_opt]
	add	rdx,32

$L$schedule_mangle_last_dec:
	add	rdx,-16
	pxor	xmm0,XMMWORD[$L$k_s63]
	call	_vpaes_schedule_transform
	movdqu	XMMWORD[rdx],xmm0


	pxor	xmm0,xmm0
	pxor	xmm1,xmm1
	pxor	xmm2,xmm2
	pxor	xmm3,xmm3
	pxor	xmm4,xmm4
	pxor	xmm5,xmm5
	pxor	xmm6,xmm6
	pxor	xmm7,xmm7
	DB	0F3h,0C3h		;repret






















ALIGN	16
_vpaes_schedule_round:


	pxor	xmm1,xmm1
DB	102,65,15,58,15,200,15
DB	102,69,15,58,15,192,15
	pxor	xmm7,xmm1


	pshufd	xmm0,xmm0,0xFF
DB	102,15,58,15,192,1




_vpaes_schedule_low_round:

	movdqa	xmm1,xmm7
	pslldq	xmm7,4
	pxor	xmm7,xmm1
	movdqa	xmm1,xmm7
	pslldq	xmm7,8
	pxor	xmm7,xmm1
	pxor	xmm7,XMMWORD[$L$k_s63]


	movdqa	xmm1,xmm9
	pandn	xmm1,xmm0
	psrld	xmm1,4
	pand	xmm0,xmm9
	movdqa	xmm2,xmm11
DB	102,15,56,0,208
	pxor	xmm0,xmm1
	movdqa	xmm3,xmm10
DB	102,15,56,0,217
	pxor	xmm3,xmm2
	movdqa	xmm4,xmm10
DB	102,15,56,0,224
	pxor	xmm4,xmm2
	movdqa	xmm2,xmm10
DB	102,15,56,0,211
	pxor	xmm2,xmm0
	movdqa	xmm3,xmm10
DB	102,15,56,0,220
	pxor	xmm3,xmm1
	movdqa	xmm4,xmm13
DB	102,15,56,0,226
	movdqa	xmm0,xmm12
DB	102,15,56,0,195
	pxor	xmm0,xmm4


	pxor	xmm0,xmm7
	movdqa	xmm7,xmm0
	DB	0F3h,0C3h		;repret













ALIGN	16
_vpaes_schedule_transform:

	movdqa	xmm1,xmm9
	pandn	xmm1,xmm0
	psrld	xmm1,4
	pand	xmm0,xmm9
	movdqa	xmm2,XMMWORD[r11]
DB	102,15,56,0,208
	movdqa	xmm0,XMMWORD[16+r11]
DB	102,15,56,0,193
	pxor	xmm0,xmm2
	DB	0F3h,0C3h		;repret



























ALIGN	16
_vpaes_schedule_mangle:

	movdqa	xmm4,xmm0
	movdqa	xmm5,XMMWORD[$L$k_mc_forward]


	add	rdx,16
	pxor	xmm4,XMMWORD[$L$k_s63]
DB	102,15,56,0,229
	movdqa	xmm3,xmm4
DB	102,15,56,0,229
	pxor	xmm3,xmm4
DB	102,15,56,0,229
	pxor	xmm3,xmm4

$L$schedule_mangle_both:
	movdqa	xmm1,XMMWORD[r10*1+r8]
DB	102,15,56,0,217
	add	r8,-16
	and	r8,0x30
	movdqu	XMMWORD[rdx],xmm3
	DB	0F3h,0C3h		;repret






global	GFp_vpaes_set_encrypt_key

ALIGN	16
GFp_vpaes_set_encrypt_key:
	mov	QWORD[8+rsp],rdi	;WIN64 prologue
	mov	QWORD[16+rsp],rsi
	mov	rax,rsp
$L$SEH_begin_GFp_vpaes_set_encrypt_key:
	mov	rdi,rcx
	mov	rsi,rdx
	mov	rdx,r8




	lea	rsp,[((-184))+rsp]
	movaps	XMMWORD[16+rsp],xmm6
	movaps	XMMWORD[32+rsp],xmm7
	movaps	XMMWORD[48+rsp],xmm8
	movaps	XMMWORD[64+rsp],xmm9
	movaps	XMMWORD[80+rsp],xmm10
	movaps	XMMWORD[96+rsp],xmm11
	movaps	XMMWORD[112+rsp],xmm12
	movaps	XMMWORD[128+rsp],xmm13
	movaps	XMMWORD[144+rsp],xmm14
	movaps	XMMWORD[160+rsp],xmm15
$L$enc_key_body:
	mov	eax,esi
	shr	eax,5
	add	eax,5
	mov	DWORD[240+rdx],eax

	mov	ecx,0
	mov	r8d,0x30
	call	_vpaes_schedule_core
	movaps	xmm6,XMMWORD[16+rsp]
	movaps	xmm7,XMMWORD[32+rsp]
	movaps	xmm8,XMMWORD[48+rsp]
	movaps	xmm9,XMMWORD[64+rsp]
	movaps	xmm10,XMMWORD[80+rsp]
	movaps	xmm11,XMMWORD[96+rsp]
	movaps	xmm12,XMMWORD[112+rsp]
	movaps	xmm13,XMMWORD[128+rsp]
	movaps	xmm14,XMMWORD[144+rsp]
	movaps	xmm15,XMMWORD[160+rsp]
	lea	rsp,[184+rsp]
$L$enc_key_epilogue:
	xor	eax,eax
	mov	rdi,QWORD[8+rsp]	;WIN64 epilogue
	mov	rsi,QWORD[16+rsp]
	DB	0F3h,0C3h		;repret

$L$SEH_end_GFp_vpaes_set_encrypt_key:

global	GFp_vpaes_encrypt

ALIGN	16
GFp_vpaes_encrypt:
	mov	QWORD[8+rsp],rdi	;WIN64 prologue
	mov	QWORD[16+rsp],rsi
	mov	rax,rsp
$L$SEH_begin_GFp_vpaes_encrypt:
	mov	rdi,rcx
	mov	rsi,rdx
	mov	rdx,r8



	lea	rsp,[((-184))+rsp]
	movaps	XMMWORD[16+rsp],xmm6
	movaps	XMMWORD[32+rsp],xmm7
	movaps	XMMWORD[48+rsp],xmm8
	movaps	XMMWORD[64+rsp],xmm9
	movaps	XMMWORD[80+rsp],xmm10
	movaps	XMMWORD[96+rsp],xmm11
	movaps	XMMWORD[112+rsp],xmm12
	movaps	XMMWORD[128+rsp],xmm13
	movaps	XMMWORD[144+rsp],xmm14
	movaps	XMMWORD[160+rsp],xmm15
$L$enc_body:
	movdqu	xmm0,XMMWORD[rdi]
	call	_vpaes_preheat
	call	_vpaes_encrypt_core
	movdqu	XMMWORD[rsi],xmm0
	movaps	xmm6,XMMWORD[16+rsp]
	movaps	xmm7,XMMWORD[32+rsp]
	movaps	xmm8,XMMWORD[48+rsp]
	movaps	xmm9,XMMWORD[64+rsp]
	movaps	xmm10,XMMWORD[80+rsp]
	movaps	xmm11,XMMWORD[96+rsp]
	movaps	xmm12,XMMWORD[112+rsp]
	movaps	xmm13,XMMWORD[128+rsp]
	movaps	xmm14,XMMWORD[144+rsp]
	movaps	xmm15,XMMWORD[160+rsp]
	lea	rsp,[184+rsp]
$L$enc_epilogue:
	mov	rdi,QWORD[8+rsp]	;WIN64 epilogue
	mov	rsi,QWORD[16+rsp]
	DB	0F3h,0C3h		;repret

$L$SEH_end_GFp_vpaes_encrypt:







ALIGN	16
_vpaes_preheat:

	lea	r10,[$L$k_s0F]
	movdqa	xmm10,XMMWORD[((-32))+r10]
	movdqa	xmm11,XMMWORD[((-16))+r10]
	movdqa	xmm9,XMMWORD[r10]
	movdqa	xmm13,XMMWORD[48+r10]
	movdqa	xmm12,XMMWORD[64+r10]
	movdqa	xmm15,XMMWORD[80+r10]
	movdqa	xmm14,XMMWORD[96+r10]
	DB	0F3h,0C3h		;repret








ALIGN	64
_vpaes_consts:
$L$k_inv:
	DQ	0x0E05060F0D080180,0x040703090A0B0C02
	DQ	0x01040A060F0B0780,0x030D0E0C02050809

$L$k_s0F:
	DQ	0x0F0F0F0F0F0F0F0F,0x0F0F0F0F0F0F0F0F

$L$k_ipt:
	DQ	0xC2B2E8985A2A7000,0xCABAE09052227808
	DQ	0x4C01307D317C4D00,0xCD80B1FCB0FDCC81

$L$k_sb1:
	DQ	0xB19BE18FCB503E00,0xA5DF7A6E142AF544
	DQ	0x3618D415FAE22300,0x3BF7CCC10D2ED9EF
$L$k_sb2:
	DQ	0xE27A93C60B712400,0x5EB7E955BC982FCD
	DQ	0x69EB88400AE12900,0xC2A163C8AB82234A
$L$k_sbo:
	DQ	0xD0D26D176FBDC700,0x15AABF7AC502A878
	DQ	0xCFE474A55FBB6A00,0x8E1E90D1412B35FA

$L$k_mc_forward:
	DQ	0x0407060500030201,0x0C0F0E0D080B0A09
	DQ	0x080B0A0904070605,0x000302010C0F0E0D
	DQ	0x0C0F0E0D080B0A09,0x0407060500030201
	DQ	0x000302010C0F0E0D,0x080B0A0904070605

$L$k_mc_backward:
	DQ	0x0605040702010003,0x0E0D0C0F0A09080B
	DQ	0x020100030E0D0C0F,0x0A09080B06050407
	DQ	0x0E0D0C0F0A09080B,0x0605040702010003
	DQ	0x0A09080B06050407,0x020100030E0D0C0F

$L$k_sr:
	DQ	0x0706050403020100,0x0F0E0D0C0B0A0908
	DQ	0x030E09040F0A0500,0x0B06010C07020D08
	DQ	0x0F060D040B020900,0x070E050C030A0108
	DQ	0x0B0E0104070A0D00,0x0306090C0F020508

$L$k_rcon:
	DQ	0x1F8391B9AF9DEEB6,0x702A98084D7C7D81

$L$k_s63:
	DQ	0x5B5B5B5B5B5B5B5B,0x5B5B5B5B5B5B5B5B

$L$k_opt:
	DQ	0xFF9F4929D6B66000,0xF7974121DEBE6808
	DQ	0x01EDBD5150BCEC00,0xE10D5DB1B05C0CE0

$L$k_deskew:
	DQ	0x07E4A34047A4E300,0x1DFEB95A5DBEF91A
	DQ	0x5F36B5DC83EA6900,0x2841C2ABF49D1E77
EXTERN	__imp_RtlVirtualUnwind

ALIGN	16
se_handler:
	push	rsi
	push	rdi
	push	rbx
	push	rbp
	push	r12
	push	r13
	push	r14
	push	r15
	pushfq
	sub	rsp,64

	mov	rax,QWORD[120+r8]
	mov	rbx,QWORD[248+r8]

	mov	rsi,QWORD[8+r9]
	mov	r11,QWORD[56+r9]

	mov	r10d,DWORD[r11]
	lea	r10,[r10*1+rsi]
	cmp	rbx,r10
	jb	NEAR $L$in_prologue

	mov	rax,QWORD[152+r8]

	mov	r10d,DWORD[4+r11]
	lea	r10,[r10*1+rsi]
	cmp	rbx,r10
	jae	NEAR $L$in_prologue

	lea	rsi,[16+rax]
	lea	rdi,[512+r8]
	mov	ecx,20
	DD	0xa548f3fc
	lea	rax,[184+rax]

$L$in_prologue:
	mov	rdi,QWORD[8+rax]
	mov	rsi,QWORD[16+rax]
	mov	QWORD[152+r8],rax
	mov	QWORD[168+r8],rsi
	mov	QWORD[176+r8],rdi

	mov	rdi,QWORD[40+r9]
	mov	rsi,r8
	mov	ecx,154
	DD	0xa548f3fc

	mov	rsi,r9
	xor	rcx,rcx
	mov	rdx,QWORD[8+rsi]
	mov	r8,QWORD[rsi]
	mov	r9,QWORD[16+rsi]
	mov	r10,QWORD[40+rsi]
	lea	r11,[56+rsi]
	lea	r12,[24+rsi]
	mov	QWORD[32+rsp],r10
	mov	QWORD[40+rsp],r11
	mov	QWORD[48+rsp],r12
	mov	QWORD[56+rsp],rcx
	call	QWORD[__imp_RtlVirtualUnwind]

	mov	eax,1
	add	rsp,64
	popfq
	pop	r15
	pop	r14
	pop	r13
	pop	r12
	pop	rbp
	pop	rbx
	pop	rdi
	pop	rsi
	DB	0F3h,0C3h		;repret


section	.pdata rdata align=4
ALIGN	4
	DD	$L$SEH_begin_GFp_vpaes_set_encrypt_key wrt ..imagebase
	DD	$L$SEH_end_GFp_vpaes_set_encrypt_key wrt ..imagebase
	DD	$L$SEH_info_GFp_vpaes_set_encrypt_key wrt ..imagebase

	DD	$L$SEH_begin_GFp_vpaes_encrypt wrt ..imagebase
	DD	$L$SEH_end_GFp_vpaes_encrypt wrt ..imagebase
	DD	$L$SEH_info_GFp_vpaes_encrypt wrt ..imagebase

section	.xdata rdata align=8
ALIGN	8
$L$SEH_info_GFp_vpaes_set_encrypt_key:
DB	9,0,0,0
	DD	se_handler wrt ..imagebase
	DD	$L$enc_key_body wrt ..imagebase,$L$enc_key_epilogue wrt ..imagebase
$L$SEH_info_GFp_vpaes_encrypt:
DB	9,0,0,0
	DD	se_handler wrt ..imagebase
	DD	$L$enc_body wrt ..imagebase,$L$enc_epilogue wrt ..imagebase
