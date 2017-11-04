BITS 64
section .note.GNU-stack noalloc noexec nowrite progbits

%define arg0 rdi
%define arg1 rsi
%define arg2 rdx

section .data
f32_2p63:
	dd 5f000000h
f64_2p63:
	dq 43E0000000000000h

section .code
STRUC ExecutionControlBlock
	.JitWriter resq 1

	.pfnEntry resq 1
	.operandStack resq 1
	.localsStack resq 1
	.cbHeap resq 1
	.memoryBase resq 1

	.cFnIndirect resq 1
	.rgFnIndirect resq 1
	.rgFnTypeIndicies resq 1
	.cFnTypeIndicies resq 1
	.rgFnPtrs resq 1
	.cFnPtrs resq 1

	; Outputs and Temps
	.stackrestore resq 1
	.retvalue resq 1
ENDSTRUC

%macro CallCFn 1
	; call the C function passed in, assuming the stack is unbalanced
	
	; balance the stack
	push r13
	mov r13, rsp
	sub rsp, 8
	and rsp, ~15
	;Do the actual call
	sub rsp, 32
	call %1 wrt ..plt
	; restore stack and go home
	mov rsp, r13
	pop r13
%endmacro

%macro BackupVMState 0
	push rdi
	push rsi
%endmacro
%macro RestoreVMState 0
	pop rsi
	pop rdi
%endmacro

extern CReentryFn
extern GrowMemory


; REGISTERS:
;	rax - top of param stack
;	rdi - param stack - 1  (volatile C calls)
;	rsi - memory base  (volatile C calls)
;	rbx	- parameter base
;	rbp - pointer to the execution control block
;	temps: rcx, rdx, r11

global ExternCallFnASM
ExternCallFnASM:
	push rbx
	push rbp
    push r12
    push r13
	push r14
    push r15

	mov rsi, [arg0 + ExecutionControlBlock.memoryBase]
	mov rbx, [arg0 + ExecutionControlBlock.localsStack]
	mov [arg0 + ExecutionControlBlock.stackrestore], rsp
	mov rax, [arg0 + ExecutionControlBlock.pfnEntry]
	mov rbp, arg0
	mov rdi, [arg0 + ExecutionControlBlock.operandStack] ; rdi == arg0
	call rax
	mov [rbp + ExecutionControlBlock.retvalue], rax
	mov [rbp + ExecutionControlBlock.operandStack], rdi
	mov [rbp + ExecutionControlBlock.localsStack], rbx

	mov eax, 1
.LDone:
    pop r15
    pop r14
    pop r13
    pop r12
	pop rbp
	pop rbx
	ret
LTrapRet:
	xor eax, eax	; return 0 for failed execution
	jmp ExternCallFnASM.LDone

global BranchTable
BranchTable:
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
	jb .LNotDefault
	xor edx, edx
	jmp .LDefault

.LNotDefault:
	;else use table
	add edx, 1						; skip the default
	shl edx, 4						; convert table offset to a byte offset
.LDefault:
	; at this point rdx is a byte offset into the vector table
	; Lets adjust the stack to deal with the blocks we're leaving
	mov r11d, [rcx+8+rdx]			; r11d = table[idx].distance
	lea rsp, [rsp+r11*8]					; adjust the stack for the blocks
	mov rax, [rdi-8]						; get the potential block return value
	pop rdi									; Restore the param stack to the right location
	; Put the top of the param stack in rax if we don't have a return value
	mov r11d, [rcx+4]
	test r11d, r11d
	jnz .LHasRetValue
	; Doesn't have a return value if we get here, so put the top param back in rax
	sub rdi, 8
	mov rax, [rdi]
.LHasRetValue:
	; The universe should be setup correctly for the target block so we just need to jump
	jmp [rcx+16+rdx]

Trap:
	mov rsp, [rbp + ExecutionControlBlock.stackrestore]
	jmp LTrapRet

global WasmToC
WasmToC:
	; Translates a wasm function call into a C function call for internal use
	; we expect the internal function # in rcx already
	; we will put the parameter base address in rdx
	; the normal JIT registers are preserved by the calling convention
	
	BackupVMState
	mov rdi, rcx	; first arg function #
	mov rdx, rsi	; third arg
	mov rsi, rbx	; put the parameter base address in rsi (second arg)
	mov rcx, rbp
	CallCFn CReentryFn
	RestoreVMState
	ret

extern CompileFn
global CallIndirectShim
CallIndirectShim:
	; ecx contains the function index
	; eax contains the type
	
	; Convert the indirect index to the function index
	; First Bounds Check
	cmp rcx, [rbp + ExecutionControlBlock.cFnIndirect]
	jae .LDoTrap
	; Now do the conversion
	mov rdx, [rbp + ExecutionControlBlock.rgFnIndirect]
	mov ecx, [rdx + rcx * 4]

	; Now bounds check the type index
	mov rdx, [rbp + ExecutionControlBlock.rgFnTypeIndicies]
	cmp rcx, [rbp + ExecutionControlBlock.cFnTypeIndicies]
	jae .LDoTrap

	mov edx, [rdx + rcx*4]	; get the callee's type
	cmp edx, eax			; check if its equal to the expected type
	jne .LDoTrap				; Trap if not				

	; Type is validated now lets do the function call
	cmp rcx, [rbp + ExecutionControlBlock.cFnPtrs]
	jae .LDoTrap
	mov rdx, [rbp + ExecutionControlBlock.rgFnPtrs]
	mov rax, [rdx + rcx*8]
	test rax, rax
	jz .LCompileFn
	jmp rax
	ud2

.LDoTrap:
	jmp Trap	; this is just for a chance to break before going

.LCompileFn:
	lea rax, [rdx + rcx*8]
	push rax
	BackupVMState
	mov esi, ecx	; second param is the function index
	mov rdi, rbp	; first param the control block
	CallCFn CompileFn
	RestoreVMState
	pop rax
	jmp [rax]
	ud2

global U64ToF32
U64ToF32:
	test rax, rax
	js .LSpecialCase
	cvtsi2ss xmm0, rax
	movd eax, xmm0
	ret
.LSpecialCase:
	mov rdx, rax
	and edx, 1			; edx store the least significant bit of our input
	shr rax, 1			; divide the input by 2
	or rax, rdx			; round to odd
	cvtsi2ss xmm0, rax	; convert input/2 -> double
	addss xmm0, xmm0	; multiply by 2
	movq rax, xmm0		; save the result in rax
	ret

global U64ToF64
U64ToF64:
	test rax, rax
	js .LSpecialCase
	cvtsi2sd xmm0, rax
	movq rax, xmm0
	ret
.LSpecialCase:
	mov rdx, rax
	and edx, 1			; edx store the least significant bit of our input
	shr rax, 1			; divide the input by 2
	or rax, rdx			; round to odd
	cvtsi2sd xmm0, rax	; convert input/2 -> double
	addsd xmm0, xmm0	; multiply by 2
	movq rax, xmm0		; save the result in rax
	ret

global F32ToU64Trunc
F32ToU64Trunc:
	movd xmm0, eax
	ucomiss xmm0, [rel f32_2p63]	; compare with 2^63
	jae .LSpecialCase
	cvttss2si rax, xmm0
	ret
.LSpecialCase:
	subss xmm0, [rel f32_2p63]	; take out the 2^63 (should reduce output by one bit)
	xor rcx, rcx
	add rcx, 1				; set lsb of rcx
	ror rcx, 1				; set msb of rcx (clear lsb)  at this point rcx == 2^63
	cvttss2si rax, xmm0		; convert to integer
	add rax, rcx			; add in the 2^63 we took out
	ret

global F64ToU64Trunc
F64ToU64Trunc:
	movq xmm0, rax
	ucomisd xmm0, [rel f64_2p63]
	jae .LSpecialCase
	cvttsd2si rax, xmm0
	ret
.LSpecialCase:
	subsd xmm0, [rel f64_2p63]	; take out the 2^63 (should reduce output by one bit)
	xor rcx, rcx
	add rcx, 1				; set lsb of rcx
	ror rcx, 1				; set msb of rcx (clear lsb) rcx == 2^63
	cvttsd2si rax, xmm0		; convert to integer
	add rax, rcx			; add back the 2^63 we removed
	ret

global GrowMemoryOp
GrowMemoryOp:
	BackupVMState
	mov rdi, rbp
	mov rsi, rax
	; Grow Memory eats an operand and returns an operand
	CallCFn GrowMemory
	RestoreVMState
	ret

