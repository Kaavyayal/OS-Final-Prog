/* Copyright (C) 2025 Ahmed Gheith and contributors.
 *
 * Use restricted to classroom projects.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "arch/saved_state.h"

	.global outb
outb:
	mov %di, %dx
	mov %sil, %al
	outb %al, %dx
	ret

	.global outl
outl:
	mov %di, %dx
	mov %esi, %eax
	outl %eax, %dx
	ret

	.global inb
inb:
	mov %di, %dx
	xor %rax, %rax
	inb %dx, %al
	ret

	.global inl
inl:
	mov %di, %dx
	xor %rax, %rax
	inl %dx, %eax
	ret

	.global get_cr0
get_cr0:
	mov %cr0, %rax
	ret

	.global get_cr4
get_cr4:
	mov %cr4, %rax
	ret

	.global set_cr4
set_cr4:
	mov %rdi, %rax
	mov %rax, %cr4
	ret

	.global enable_sse
enable_sse:
	mov %cr4, %rax
	and $0xfffb, %ax
	or $0x602, %ax
	mov %rax, %cr4
	ret

	.global rdfsbase
rdfsbase:
	rdfsbase %rax
	ret

	.global wrfsbase
wrfsbase:
	wrfsbase %rdi
	ret

	.global rdgsbase
rdgsbase:
	rdgsbase %rax
	ret

	.global wrgsbase
wrgsbase:
	wrgsbase %rdi
	ret

	.global memcpy
memcpy:
	cld
	mov %rdi, %rax
	mov %rdx, %rcx
	rep movsb
	ret

	.global memset
memset:
	cld
	mov %rdi, %r8
	mov %esi, %eax
	mov %rdx, %rcx
	rep stosb
	mov %r8, %rax
	ret

	.global strlen
strlen:
	mov %rdi, %rax
	mov $0, %rcx
1:
	movb (%rax), %r11b
	cmpb $0, %r11b
	je 1f
	inc %rax
	inc %rcx
	jmp 1b
1:
	mov %rcx, %rax
	ret

	.global streq
streq:
	movb (%rdi), %al
	cmpb (%rsi), %al
	
	je 1f
	// not equal, return 0
	xor %rax, %rax
	ret

	// equal
1:
	cmp $0, %al
	je 1f
	inc %rdi
	inc %rsi
	jmp streq     // keep going

1:
	movq $1, %rax     // equal, return 1
	ret

	.global pause
pause:
	pause
	ret

	.global get_flags
get_flags:
	pushf
	pop %rax
	ret

// The CCB fields the entry code uses (ccb.cc checks the offsets).
        .set CCB_STACK_TOP, 0x10
        .set CCB_USER_RSP, 0x18
        .set CCB_SAVED_STATE, 0x20      // the current thread's SavedState *

        .set KERNEL_GS_BASE, 0xC0000102

// SAVE_STATE: saves all of the user's state in its SavedState.
//   Q: Where does it find the current thread's SavedState?
//   Q: Why load the kernel's x87/SSE settings after saving?
//   Q: What does RAX point to when it's done?
.macro SAVE_STATE
        push %rax
        mov %gs:CCB_SAVED_STATE, %rax
        popq SS_RAX(%rax)
        mov %rcx, SS_RCX(%rax)
        mov %rdx, SS_RDX(%rax)
        mov %rsi, SS_RSI(%rax)
        mov %rdi, SS_RDI(%rax)
        mov %r8, SS_R8(%rax)
        mov %r9, SS_R9(%rax)
        mov %r10, SS_R10(%rax)
        mov %r11, SS_R11(%rax)

        mov %rbx, SS_RBX(%rax)
        mov %rbp, SS_RBP(%rax)
        mov %r12, SS_R12(%rax)
        mov %r13, SS_R13(%rax)
        mov %r14, SS_R14(%rax)
        mov %r15, SS_R15(%rax)

        fxsave64 SS_FPU(%rax)
        // The kernel's x87/SSE control settings, not the user's.
        fninit
        ldmxcsr kernel_mxcsr(%rip)

        rdfsbase %rcx
        mov %rcx, SS_FS_BASE(%rax)
        // The user's GS base: swapgs left it in KERNEL_GS_BASE.
        mov %rax, %rsi
        mov $KERNEL_GS_BASE, %ecx
        rdmsr
        shl $32, %rdx
        or %rdx, %rax
        mov %rax, SS_GS_BASE(%rsi)
        mov %rsi, %rax
.endm

// The system call entry point (IA32_LSTAR).
//   Q: Whose stack and GS is it on when it starts?
//   Q: Where are the user's RIP and RFLAGS?
// Returns through the full restore path so exec can reset TLS and registers.
        .global syscallEntry_
syscallEntry_:
        swapgs
        mov %rsp, %gs:CCB_USER_RSP
        mov %gs:CCB_STACK_TOP, %rsp

        SAVE_STATE
        mov SS_RCX(%rax), %rcx
        mov %rcx, SS_RIP(%rax)
        mov SS_R11(%rax), %rcx
        mov %rcx, SS_RFLAGS(%rax)
        mov %gs:CCB_USER_RSP, %rcx
        mov %rcx, SS_RSP(%rax)
        // Still the thread's page tables: the kernel runs on them too.

        // RSP is stack_top again, 16-byte aligned for the call
        call syscallEntry
        mov %rax, %rdi
        jmp restore_context_

// The page fault handler (an interrupt gate).
//   Q: Where is the error code, and what's above it?
//   Q: Why does no page fault come back here?
        .global pageFaultHandler_
pageFaultHandler_:
        testb $3, 16(%rsp)      // CS RPL: from user mode?
        jz 1f

        // From user mode: save the thread's state and clear the user's AC.
        swapgs
        clac
        cld
        SAVE_STATE
        mov 8(%rsp), %rcx       // the faulting instruction, it runs again
        mov %rcx, SS_RIP(%rax)
        mov 24(%rsp), %rcx
        mov %rcx, SS_RFLAGS(%rax)
        mov 32(%rsp), %rcx
        mov %rcx, SS_RSP(%rax)
        // Still the thread's process's page tables.

        mov %cr2, %rdi
        mov (%rsp), %rsi        // error code
        // 6 words pushed on stack_top: 16-byte aligned for the call.
        call userPageFault
        ud2                     // doesn't return

1:      // From the kernel: whatever the registers had doesn't matter, it
        // panics
        mov %cr2, %rdi
        mov (%rsp), %rsi        // error code
        mov 8(%rsp), %rdx       // RIP
        mov 24(%rsp), %rcx      // RFLAGS
        and $-16, %rsp
        call kernelPageFault
        ud2                     // doesn't return

        .global generalProtectionHandler_
generalProtectionHandler_:
        testb $3,16(%rsp)
        jz 9f
        swapgs
        clac
        cld
        SAVE_STATE
        mov 8(%rsp),%rcx
        mov %rcx,SS_RIP(%rax)
        mov 24(%rsp),%rcx
        mov %rcx,SS_RFLAGS(%rax)
        mov 32(%rsp),%rcx
        mov %rcx,SS_RSP(%rax)
        call userGeneralProtection
        ud2
9:      ud2

	// call_on_stack_(arg, fn, stack_top): runs fn(arg) on another stack.
	//   Q: Why must stack_top be 16-byte aligned?
	//   Q: How does it get back to the caller's stack?
	.global call_on_stack_
call_on_stack_:
	push %rbp
	mov %rsp, %rbp      // callee-saved, survives the call
	mov %rdx, %rsp
	call *%rsi          // arg is already in %rdi
	mov %rbp, %rsp
	pop %rbp
	ret

// INTERRUPT_HANDLER name, fn: an interrupt gate's entry that calls the C++
// fn() with the interrupted code's registers and x87/SSE state saved.
//   Q: Why save the x87/SSE state here?
//   Q: Why does -mno-red-zone matter to an interrupt handler?
.macro INTERRUPT_HANDLER name, fn
	.global \name
\name:
        // From user mode: switch to the kernel's GS and clear the user's AC.
        testb $3, 8(%rsp)
        jz 1f
        swapgs
        clac
1:
        cld
        push %rax
        push %rcx
        push %rdx
        push %rsi
        push %rdi
        push %r8
        push %r9
        push %r10
        push %r11
        push %rbx

        // fxsave needs 512 bytes, 16-byte aligned; aligns RSP for the call.
        mov %rsp, %rbx    // callee-saved, survives the call
        sub $512, %rsp
        and $-16, %rsp
        fxsave64 (%rsp)
        // The kernel's x87/SSE settings, not the interrupted code's.
        fninit
        ldmxcsr kernel_mxcsr(%rip)
        call \fn
        fxrstor64 (%rsp)
        mov %rbx, %rsp

        pop %rbx
        pop %r11
        pop %r10
        pop %r9
        pop %r8
        pop %rdi
        pop %rsi
        pop %rdx
        pop %rcx
        pop %rax

        testb $3, 8(%rsp)
        jz 1f
        swapgs
1:
        iretq
.endm

	// The TLB shootdown IPI handler.
	INTERRUPT_HANDLER tlbHandler_, tlbHandler

	// The APIC timer's handler.
	INTERRUPT_HANDLER apitKernelHandler_, apitHandler

        // Only user mode is preemptible: kernel coroutines own their stacks.
        .global apitHandler_
apitHandler_:
        testb $3, 8(%rsp)
        jz apitKernelHandler_
        swapgs
        clac
        cld
        SAVE_STATE
        mov 0(%rsp), %rcx
        mov %rcx, SS_RIP(%rax)
        mov 16(%rsp), %rcx
        mov %rcx, SS_RFLAGS(%rax)
        mov 24(%rsp), %rcx
        mov %rcx, SS_RSP(%rax)
        and $-16, %rsp
        call userTimerInterrupt
        ud2

	// restore_context_(state): see machine.h.
	//   Q: In what order does it restore FS, GS and the registers?
	//   Q: What goes into the iretq frame?
	.global restore_context_
restore_context_:
        fxrstor64 SS_FPU(%rdi)

        // FS base, and KERNEL_GS_BASE = the user's GS (swapgs swaps it).
        mov SS_GS_BASE(%rdi), %rax
        mov %rax, %rdx
        shr $32, %rdx
        mov $KERNEL_GS_BASE, %ecx
        wrmsr
        mov SS_FS_BASE(%rdi), %rax
        wrfsbase %rax

        // the iretq frame
        pushq $0x3b             // GDT::USER_DS
        pushq SS_RSP(%rdi)
        pushq SS_RFLAGS(%rdi)
        pushq $0x43             // GDT::USER_CS
        pushq SS_RIP(%rdi)

        mov SS_RBX(%rdi), %rbx
        mov SS_RBP(%rdi), %rbp
        mov SS_R12(%rdi), %r12
        mov SS_R13(%rdi), %r13
        mov SS_R14(%rdi), %r14
        mov SS_R15(%rdi), %r15

        mov SS_RAX(%rdi), %rax
        mov SS_RCX(%rdi), %rcx
        mov SS_RDX(%rdi), %rdx
        mov SS_R8(%rdi), %r8
        mov SS_R9(%rdi), %r9
        mov SS_R10(%rdi), %r10
        mov SS_R11(%rdi), %r11
        mov SS_RSI(%rdi), %rsi
        mov SS_RDI(%rdi), %rdi  // last, it's the pointer

        swapgs                  // GS = the user's, KERNEL_GS_BASE = the CCB
        iretq

	// The MXCSR kernel code runs with: SSE exceptions masked.
	//   Q: Why mask the exceptions instead of handling them?
	.section .rodata
	.balign 4
	.global kernel_mxcsr
kernel_mxcsr:
	.long 0x1F80

	// fxrstor's image of a fresh x87/SSE state (see saved_state.h).
	.balign 16
	.global clean_fpu
clean_fpu:
	.word 0x037F             // FCW
	.fill 22, 1, 0
	.long 0x1F80             // MXCSR, at 24
	.fill 512 - 28, 1, 0
	.text

	    .global spuriousHandler_
spuriousHandler_:
        iretq

	.section .note.GNU-stack,"",@progbits
	

