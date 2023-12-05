#include "userprog/gdt.h"
#include <debug.h>
#include "userprog/tss.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "intrinsic.h"

/* Global Descriptor Table (GDT) 정의.
 *
 * GDT는 x86-64 특정 구조로, 시스템 내의 모든 프로세스가 사용할 수 있는 세그먼트를 정의합니다.
 * 권한에 따라 다르게 적용됩니다. 현대의 운영 시스템에서는 Local Descriptor Table (LDT)는 사용되지 않습니다.
 *
 * GDT의 각 항목은 테이블 내의 바이트 오프셋으로 알려져 있으며, 세그먼트를 식별합니다.
 * 우리의 목적을 위해서는 세 가지 타입의 세그먼트가 관심사입니다: 코드, 데이터, 그리고 TSS 또는 Task-State Segment 디스크립터.
 * 전자 두 타입은 그들이 나타내는 것과 정확히 같습니다. TSS는 주로 인터럽트에서 스택 전환을 위해 사용됩니다. */


struct segment_desc {
	unsigned lim_15_0 : 16;
	unsigned base_15_0 : 16;
	unsigned base_23_16 : 8;
	unsigned type : 4;
	unsigned s : 1;
	unsigned dpl : 2;
	unsigned p : 1;
	unsigned lim_19_16 : 4;
	unsigned avl : 1;
	unsigned l : 1;
	unsigned db : 1;
	unsigned g : 1;
	unsigned base_31_24 : 8;
};

struct segment_descriptor64 {
	unsigned lim_15_0 : 16;
	unsigned base_15_0 : 16;
	unsigned base_23_16 : 8;
	unsigned type : 4;
	unsigned s : 1;
	unsigned dpl : 2;
	unsigned p : 1;
	unsigned lim_19_16 : 4;
	unsigned avl : 1;
	unsigned rsv1 : 2;
	unsigned g : 1;
	unsigned base_31_24 : 8;
	uint32_t base_63_32;
	unsigned res1 : 8;
	unsigned clear : 8;
	unsigned res2 : 16;
};

#define SEG64(type, base, lim, dpl) (struct segment_desc) \
{ ((lim) >> 12) & 0xffff, (base) & 0xffff, ((base) >> 16) & 0xff, \
	type, 1, dpl, 1, (unsigned) (lim) >> 28, 0, 1, 0, 1, \
	(unsigned) (base) >> 24 }

static struct segment_desc gdt[SEL_CNT] = {
	[SEL_NULL >> 3] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	[SEL_KCSEG >> 3] = SEG64 (0xa, 0x0, 0xffffffff, 0),
	[SEL_KDSEG >> 3] = SEG64 (0x2, 0x0, 0xffffffff, 0),
	[SEL_UDSEG >> 3] = SEG64 (0x2, 0x0, 0xffffffff, 3),
	[SEL_UCSEG >> 3] = SEG64 (0xa, 0x0, 0xffffffff, 3),
	[SEL_TSS >> 3] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	[6] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	[7] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

struct desc_ptr gdt_ds = {
	.size = sizeof(gdt) - 1,
	.address = (uint64_t) gdt
};

/* 적절한 GDT를 설정합니다. 부트스트랩 로더의 GDT는 사용자 모드 선택자나 TSS를 포함하지 않았지만, 이제는 둘 다 필요합니다. */
void
gdt_init (void) {
	/* Initialize GDT. */
	struct segment_descriptor64 *tss_desc =
		(struct segment_descriptor64 *) &gdt[SEL_TSS >> 3];
	struct task_state *tss = tss_get ();

	*tss_desc = (struct segment_descriptor64) {
		.lim_15_0 = (uint64_t) (sizeof (struct task_state)) & 0xffff,
		.base_15_0 = (uint64_t) (tss) & 0xffff,
		.base_23_16 = ((uint64_t) (tss) >> 16) & 0xff,
		.type = 0x9,
		.s = 0,
		.dpl = 0,
		.p = 1,
		.lim_19_16 = ((uint64_t)(sizeof (struct task_state)) >> 16) & 0xf,
		.avl = 0,
		.rsv1 = 0,
		.g = 0,
		.base_31_24 = ((uint64_t)(tss) >> 24) & 0xff,
		.base_63_32 = ((uint64_t)(tss) >> 32) & 0xffffffff,
		.res1 = 0,
		.clear = 0,
		.res2 = 0
	};

	lgdt (&gdt_ds);
	/* reload segment registers */
	asm volatile("movw %%ax, %%gs" :: "a" (SEL_UDSEG));
	asm volatile("movw %%ax, %%fs" :: "a" (0));
	asm volatile("movw %%ax, %%es" :: "a" (SEL_KDSEG));
	asm volatile("movw %%ax, %%ds" :: "a" (SEL_KDSEG));
	asm volatile("movw %%ax, %%ss" :: "a" (SEL_KDSEG));
	asm volatile("pushq %%rbx\n"
			"movabs $1f, %%rax\n"
			"pushq %%rax\n"
			"lretq\n"
			"1:\n" :: "b" (SEL_KCSEG):"cc","memory");
	/* Kill the local descriptor table */
	lldt (0);
}
