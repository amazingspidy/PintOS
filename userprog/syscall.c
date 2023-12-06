#include "userprog/syscall.h"

#include <stdio.h>
#include <syscall-nr.h>

#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/loader.h"
#include "threads/thread.h"
#include "userprog/gdt.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

/* 시스템 콜.
 *
 * 이전에는 시스템 콜 서비스가 인터럽트 핸들러에 의해 처리되었습니다
 * (예: 리눅스에서 int 0x80). 그러나 x86-64에서는 제조업체가
 * 시스템 콜을 요청하기 위한 효율적인 경로인 `syscall` 명령어를 제공합니다.
 *
 * syscall 명령어는 모델 특정 레지스터(Model Specific Register, MSR)에서 값을
 * 읽어 작동합니다. 자세한 내용은 매뉴얼을 참조하세요. */

#define MSR_STAR 0xc0000081         /* 세그먼트 셀렉터 MSR */
#define MSR_LSTAR 0xc0000082        /* 롱 모드 SYSCALL 대상 */
#define MSR_SYSCALL_MASK 0xc0000084 /* eflags에 대한 마스크 */

void syscall_init(void) {
    write_msr(MSR_STAR,
              ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG) << 32);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* 인터럽트 서비스 루틴은 syscall_entry가 사용자 영역 스택을 커널
     * 모드 스택으로 전환할 때까지 어떠한 인터럽트도 처리하지 않아야 합니다.
     * 따라서, FLAG_FL을 마스킹했습니다. */
    write_msr(MSR_SYSCALL_MASK,
              FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* 주요 시스템 콜 인터페이스 */
// void
// syscall_handler (struct intr_frame *f UNUSED) {
// 	// TODO: 여기에 구현이 들어갑니다.
// 	printf ("system call!\n");
// 	thread_exit ();
// }

void check_address(void *addr) {
    if (!is_user_vaddr(addr)) {
    }
}
void syscall_handler(struct intr_frame *f) {
    // 시스템 콜 번호를 RAX 레지스터로부터 읽어옵니다.
    // check_address(&f->rsp);

    int syscall_number = f->R.rax;

    // 시스템 콜 결과를 저장할 변수
    int syscall_result = -1;

    // 시스템 콜 번호에 따라 적절한 처리 수행
    switch (syscall_number) {
        case SYS_HALT:
            // ... halt 처리 ...
            power_off();
            break;
        case SYS_EXIT:
            // ... exit 처리 ...
            // exit(f->R.rdi);  // 예를 들어, exit 시스템 콜의 인자는 RDI
            // 레지스터에 저장됩니다.
            break;
        case SYS_FORK:
            break;
        case SYS_EXEC:
            break;
        case SYS_WAIT:
            break;
        case SYS_CREATE:
            break;
        case SYS_REMOVE:
            break;
        case SYS_OPEN:
            break;
        case SYS_FILESIZE:
            break;
        case SYS_READ:
            break;
        case SYS_WRITE:
            break;
        case SYS_TELL:
            break;
        case SYS_CLOSE:
            break;
        default:
            printf("Unknown system call number: %d\n", syscall_number);
            break;
    }

    // 시스템 콜 처리 결과를 RAX 레지스터에 저장
    f->R.rax = syscall_result;

    // 시스템 콜이 종료된 후의 동작을 수행할 수 있습니다.
    // 예를 들어, 스레드를 종료시키는 대신 다른 작업을 수행할 수 있습니다.
    thread_exit();
}