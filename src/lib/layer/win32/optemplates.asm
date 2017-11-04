_DATA SEGMENT
f32_2p63:
	dd 5f000000h
f64_2p63:
	dq 43E0000000000000h
_TEXT SEGMENT

ExecutionControlBlock STRUCT
	JitWriter dq ?

	pfnEntry dq ?
	operandStack dq ?
	localsStack dq ?
	cbHeap dq ?
	memoryBase dq ?

	cFnIndirect dq ?
	rgfnIndirect dq ?
	rgFnTypeIndicies dq ?
	cFnTypeIndicies dq ?
	rgFnPtrs dq ?
	cFnPtrs dq ?

	; Outputs and Temps
	stackrestore dq ?
	retvalue dq ?
ExecutionControlBlock ENDS

CallCFn	MACRO fn
	; call the C function passed in, assuming the stack is unbalanced
	
	; balance the stack
	push r13
	mov r13, rsp
	sub rsp, 8
	and rsp, not 15
	;Do the actual call
	sub rsp, 32
	call fn
	; restore stack and go home
	mov rsp, r13
	pop r13
ENDM

CReentryFn PROTO
GrowMemory PROTO


; REGISTERS:
;	rax - top of param stack
;	rdi - param stack - 1
;	rsi - memory base
;	rbx	- parameter base
;	rbp - pointer to the execution control block
;	temps: rcx, rdx, r11

ExternCallFnASM PROC pctl : ptr ExecutionControlBlock
	push rdi
	push rsi
	push rbx
	push rbp
	
	mov rdi, (ExecutionControlBlock PTR [rcx]).operandStack
	mov rsi, (ExecutionControlBlock PTR [rcx]).memoryBase
	mov rbx, (ExecutionControlBlock PTR [rcx]).localsStack
	mov (ExecutionControlBlock PTR [rcx]).stackrestore, rsp
	mov rbp, rcx
	mov rax, (ExecutionControlBlock PTR [rcx]).pfnEntry
	call rax
	mov (ExecutionControlBlock PTR [rbp]).retvalue, rax
	mov (ExecutionControlBlock PTR [rbp]).operandStack, rdi
	mov (ExecutionControlBlock PTR [rbp]).localsStack, rbx

	mov eax, 1
LDone:
	pop rbp
	pop rbx
	pop rsi
	pop rdi
	ret
LTrapRet::
	xor eax, eax	; return 0 for failed execution
	jmp LDone
ExternCallFnASM ENDP

BranchTable PROC
	; jump to an argument in the table or default
	;	note: this is a candidate for more optimization but for now we will always perform a jump table
	;
	;	rax - table index
	;	rcx - table pointer
	;	edx - temp
	;
	;	Table Format:
	;		dword count
	;		dword fReturnVal
	;		--default_target--
	;		qword distance (how many blocks we're jumping)
	;		qword target_addr
	;		--table_targets---
	;		.... * count
	
	mov edx, eax		; backup the table index
	mov rax, [rdi]		; store the block return value (or garbage)

	cmp edx, [rcx]
	jb LNotDefault
	xor edx, edx
	jmp LDefault

LNotDefault:
	;else use table
	add edx, 1						; skip the default
	shl edx, 4						; convert table offset to a byte offset
LDefault:
	; at this point rdx is a byte offset into the vector table
	; Lets adjust the stack to deal with the blocks we're leaving
	mov r11d, dword ptr [rcx+8+rdx]			; r11d = table[idx].distance
	lea rsp, [rsp+r11*8]					; adjust the stack for the blocks
	mov rax, [rdi-8]						; get the potential block return value
	pop rdi									; Restore the param stack to the right location
	; Put the top of the param stack in rax if we don't have a return value
	mov r11d, dword ptr [rcx+4]
	test r11d, r11d
	jnz LHasRetValue
	; Doesn't have a return value if we get here, so put the top param back in rax
	sub rdi, 8
	mov rax, [rdi]
LHasRetValue:
	; The universe should be setup correctly for the target block so we just need to jump
	jmp [rcx+16+rdx]
BranchTable ENDP

Trap PROC
	mov rsp, (ExecutionControlBlock PTR [rbp]).stackrestore
	jmp LTrapRet
Trap ENDP

WasmToC PROC
	; Translates a wasm function call into a C function call for internal use
	; we expect the internal function # in rcx already
	; we will put the parameter base address in rdx
	; the normal JIT registers are preserved by the calling convention
	
	mov rdx, rbx	; put the parameter base address in rdx (second arg)
	mov r8, rsi
	mov r9, rbp
	; reserve space on stack

	sub rsp, 32
	call CReentryFn
	add rsp, 32
	ret
WasmToC ENDP

CompileFn PROTO
CallIndirectShim PROC
	; ecx contains the function index
	; eax contains the type
	
	; Convert the indirect index to the function index
	; First Bounds Check
	cmp rcx, (ExecutionControlBlock PTR [rbp]).cFnIndirect
	jae LDoTrap
	; Now do the conversion
	mov rdx, (ExecutionControlBlock PTR [rbp]).rgFnIndirect
	mov ecx, [rdx + rcx * 4]

	; Now bounds check the type index
	mov rdx, (ExecutionControlBlock PTR [rbp]).rgFnTypeIndicies
	cmp rcx, (ExecutionControlBlock PTR [rbp]).cFnTypeIndicies
	jae LDoTrap

	mov edx, [rdx + rcx*4]	; get the callee's type
	cmp edx, eax			; check if its equal to the expected type
	jne LDoTrap				; Trap if not				

	; Type is validated now lets do the function call
	cmp rcx, (ExecutionControlBlock PTR [rbp]).cFnPtrs
	jae LDoTrap
	mov rdx, (ExecutionControlBlock PTR [rbp]).rgFnPtrs
	mov rax, [rdx + rcx*8]
	test rax, rax
	jz LCompileFn
	jmp rax
	ud2

LDoTrap:
	jmp Trap	; this is just for a chance to break before going

LCompileFn:
	lea rax, [rdx + rcx*8]
	push rax
	mov edx, ecx	; second param is the function index
	mov rcx, rbp	; first param the control block
	CallCFn CompileFn
	pop rax
	jmp qword ptr [rax]
	ud2
CallIndirectShim ENDP

U64ToF32 PROC
	test rax, rax
	js LSpecialCase
	cvtsi2ss xmm0, rax
	movd rax, xmm0
	ret
LSpecialCase:
	mov rdx, rax
	and edx, 1			; edx store the least significant bit of our input
	shr rax, 1			; divide the input by 2
	or rax, rdx			; round to odd
	cvtsi2ss xmm0, rax	; convert input/2 -> double
	addss xmm0, xmm0	; multiply by 2
	movq rax, xmm0		; save the result in rax
	ret
U64ToF32 ENDP

U64ToF64 PROC
	test rax, rax
	js LSpecialCase
	cvtsi2sd xmm0, rax
	movq rax, xmm0
	ret
LSpecialCase:
	mov rdx, rax
	and edx, 1			; edx store the least significant bit of our input
	shr rax, 1			; divide the input by 2
	or rax, rdx			; round to odd
	cvtsi2sd xmm0, rax	; convert input/2 -> double
	addsd xmm0, xmm0	; multiply by 2
	movq rax, xmm0		; save the result in rax
	ret
U64ToF64 ENDP

F32ToU64Trunc PROC
	movd xmm0, eax
	ucomiss xmm0, dword ptr [f32_2p63]	; compare with 2^63
	jae LSpecialCase
	cvttss2si rax, xmm0
	ret
LSpecialCase:
	subss xmm0, dword ptr [f32_2p63]	; take out the 2^63 (should reduce output by one bit)
	xor rcx, rcx
	add rcx, 1				; set lsb of rcx
	ror rcx, 1				; set msb of rcx (clear lsb)  at this point rcx == 2^63
	cvttss2si rax, xmm0		; convert to integer
	add rax, rcx			; add in the 2^63 we took out
	ret
F32ToU64Trunc ENDP

F64ToU64Trunc PROC
	movq xmm0, rax
	ucomisd xmm0, qword ptr [f64_2p63]
	jae LSpecialCase
	cvttsd2si rax, xmm0
	ret
LSpecialCase:
	subsd xmm0, qword ptr [f64_2p63]	; take out the 2^63 (should reduce output by one bit)
	xor rcx, rcx
	add rcx, 1				; set lsb of rcx
	ror rcx, 1				; set msb of rcx (clear lsb) rcx == 2^63
	cvttsd2si rax, xmm0		; convert to integer
	add rax, rcx			; add back the 2^63 we removed
	ret
F64ToU64Trunc ENDP

GrowMemoryOp PROC
	mov rcx, rbp
	mov rdx, rax
	; Grow Memory eats an operand and returns an operand
	CallCFn GrowMemory
	ret
GrowMemoryOp ENDP

_TEXT ENDS

END