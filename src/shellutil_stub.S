@ Import stub for sceShellUtilRequestColdReset, which vita-headers lacks.
@ Based on vita-libs-gen-2 output for SceShellSvc/SceShellUtil NID 0x636544FB.
@ Weak (flag 8) like the SDK's SceShellSvc_stub_weak; with a strict import
@ the plugin failed to load from *main.

.arch armv7a

.section .vitalink.fstubs.SceShellUtil,"ax",%progbits

	.align 4
	.global sceShellUtilRequestColdReset
	.type sceShellUtilRequestColdReset, %function
sceShellUtilRequestColdReset:
	.word 0x00000008
	.word 0xD2B1C8AE
	.word 0x636544FB
	.align 4
