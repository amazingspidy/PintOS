#include "userprog/tss.h"
#include <debug.h>
#include <stddef.h>
#include "userprog/gdt.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "intrinsic.h"

/* 작업 상태 세그먼트 (Task-State Segment, TSS).
 *
 * TSS는 x86-64 특정 구조로, 프로세서에 내장된 멀티태스킹을 지원하기 위해 '작업'을 정의하는 데 사용됩니다.
 * 그러나 이식성, 속도, 유연성 등 여러 이유로 대부분의 x86-64 OS는 TSS를 거의 완전히 무시합니다. 우리도 예외는 아닙니다.
 *
 * 불행히도, TSS를 사용하지 않고는 할 수 없는 한 가지가 있습니다: 사용자 모드에서 발생한 인터럽트에 대한 스택 전환입니다.
 * 사용자 모드(링 3)에서 인터럽트가 발생하면 프로세서는 인터럽트 처리를 위해 사용할 스택을 결정하기 위해 현재 TSS의 rsp0 멤버를 참조합니다.
 * 따라서, TSS를 생성하고 적어도 이 필드들을 초기화해야 하며, 이 파일이 정확히 그렇게 합니다.
 *
 * 인터럽트 또는 트랩 게이트에 의해 인터럽트가 처리될 때(우리가 처리하는 모든 인터럽트에 해당) x86-64 프로세서는 다음과 같이 작동합니다:
 *
 *    - 인터럽트에 의해 중단된 코드가 인터럽트 핸들러와 같은 링에 있는 경우, 스택 전환이 발생하지 않습니다.
 *      이는 커널에서 실행되는 동안 발생하는 인터럽트에 해당합니다. 이 경우 TSS의 내용은 관련이 없습니다.
 *
 *    - 인터럽트가 중단된 코드가 핸들러와 다른 링에 있는 경우, 프로세서는 새 링에 지정된 TSS의 스택으로 전환합니다.
 *      이는 사용자 공간에서 발생하는 인터럽트에 해당합니다. 이미 사용 중인 스택으로 전환하여 데이터가 손상되지 않도록 하는 것이 중요합니다.
 *      사용자 공간에서 실행 중이기 때문에 현재 프로세스의 커널 스택이 사용되지 않고 있다는 것을 알고 있으므로 항상 그것을 사용할 수 있습니다.
 *      따라서 스케줄러가 스레드를 전환할 때 TSS의 스택 포인터도 새 스레드의 커널 스택을 가리키도록 변경합니다.
 *      (호출은 thread.c의 schedule에 있습니다.) */

/* 커널 TSS. */
struct task_state *tss;

/* 커널 TSS를 초기화합니다. */
void
tss_init (void) {
	/* 우리의 TSS는 호출 게이트나 작업 게이트에서 사용되지 않으므로, 참조되는 몇 가지 필드만 초기화합니다. */
	tss = palloc_get_page (PAL_ASSERT | PAL_ZERO);
	tss_update (thread_current ());
}

/* 커널 TSS를 반환합니다. */
struct task_state *
tss_get (void) {
	ASSERT (tss != NULL);
	return tss;
}

/* TSS의 링 0 스택 포인터를 스레드 스택의 끝을 가리키도록 설정합니다. */
void
tss_update (struct thread *next) {
	ASSERT (tss != NULL);
	tss->rsp0 = (uint64_t) next + PGSIZE;
}
