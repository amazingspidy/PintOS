#include "userprog/exception.h"

#include <inttypes.h>
#include <stdio.h>

#include "intrinsic.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/gdt.h"

/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill(struct intr_frame *);
static void page_fault(struct intr_frame *);

/* 사용자 프로그램에 의해 발생할 수 있는 인터럽트에 대한 핸들러를 등록하는 함수.
   실제 Unix 같은 OS에서는 이러한 인터럽트 대부분을 시그널의 형태로 사용자
   프로세스에 전달하지만, 여기에서는 그러한 시그널을 구현하지 않습니다. 대신,
   간단히 사용자 프로세스를 종료시킵니다. 페이지 폴트는 예외로, 여기에서는 다른
   예외와 같은 방식으로 처리되지만 가상 메모리를 구현하기 위해서는 변경될 필요가
   있습니다. */
void exception_init(void) {
    /* 사용자 프로그램에서 명시적으로 발생시킬 수 있는 예외들을 등록합니다. */
    /* These exceptions can be raised explicitly by a user program,
       e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
       we set DPL==3, meaning that user programs are allowed to
       invoke them via these instructions. */
    intr_register_int(3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
    intr_register_int(4, 3, INTR_ON, kill, "#OF Overflow Exception");
    intr_register_int(5, 3, INTR_ON, kill,
                      "#BR BOUND Range Exceeded Exception");

    /* 사용자 프로세스가 INT 명령어를 통해 호출할 수 없도록 DPL==0으로 설정된
     * 예외들을 등록합니다. */
    /* These exceptions have DPL==0, preventing user processes from
       invoking them via the INT instruction.  They can still be
       caused indirectly, e.g. #DE can be caused by dividing by
       0.  */
    intr_register_int(0, 0, INTR_ON, kill, "#DE Divide Error");
    intr_register_int(1, 0, INTR_ON, kill, "#DB Debug Exception");
    intr_register_int(6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
    intr_register_int(7, 0, INTR_ON, kill,
                      "#NM Device Not Available Exception");
    intr_register_int(11, 0, INTR_ON, kill, "#NP Segment Not Present");
    intr_register_int(12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
    intr_register_int(13, 0, INTR_ON, kill, "#GP General Protection Exception");
    intr_register_int(16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
    intr_register_int(19, 0, INTR_ON, kill,
                      "#XF SIMD Floating-Point Exception");

    /* 대부분의 예외는 인터럽트가 활성화된 상태에서 처리할 수 있습니다.
       페이지 폴트의 경우, 오류 주소가 CR2에 저장되어 있어야 하므로 인터럽트를
       비활성화합니다. */
    intr_register_int(14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* 예외 통계를 출력하는 함수. */
void exception_print_stats(void) {
    printf("Exception: %lld page faults\n", page_fault_cnt);
}

/* 사용자 프로세스에 의해 발생한 것으로 추정되는 예외를 처리하는 핸들러 함수. */
static void kill(struct intr_frame *f) {
    /* 이 인터럽트는 사용자 프로세스에 의해 발생한 것으로 추정됩니다.
       예를 들어, 프로세스가 매핑되지 않은 가상 메모리에 접근하려고 시도했을 수
       있습니다 (페이지 폴트). 지금은 단순히 사용자 프로세스를 종료합니다.
       나중에는 커널에서 페이지 폴트를 처리할 것입니다. 실제 Unix와 같은 운영
       시스템들은 대부분의 예외를 시그널을 통해 프로세스에 전달하지만,
       여기에서는 그러한 시그널을 구현하지 않습니다. */

    /* 인터럽트 프레임의 코드 세그먼트 값으로 예외의 원인을 판단합니다. */
    switch (f->cs) {
        case SEL_UCSEG:
            /* 사용자 코드 세그먼트이므로, 예상대로 사용자 예외입니다. 사용자
             * 프로세스를 종료합니다. */
            printf("%s: dying due to interrupt %#04llx (%s).\n", thread_name(),
                   f->vec_no, intr_name(f->vec_no));
            intr_dump_frame(f);
            thread_exit();

        case SEL_KCSEG:
            /* 커널의 코드 세그먼트, 이는 커널 버그를 나타냅니다.
커널 코드는 예외를 발생시키지 않아야 합니다. (페이지 폴트는
커널 예외를 일으킬 수 있지만, 여기에 도달해서는 안 됩니다.)
커널을 패닉 상태로 만듭니다. */
            intr_dump_frame(f);
            PANIC("Kernel bug - unexpected interrupt in kernel");

        default:
            /* 다른 코드 세그먼트? 일어나지 않아야 합니다. 커널을 패닉 상태로
             * 만듭니다. */
            printf("Interrupt %#04llx (%s) in unknown segment %04x\n",
                   f->vec_no, intr_name(f->vec_no), f->cs);
            thread_exit();
    }
}

/* 페이지 폴트 핸들러. 이것은 가상 메모리를 구현하기 위해 채워져야 하는
   스켈레톤입니다. 프로젝트 2의 일부 솔루션도 이 코드를 수정해야 할 수 있습니다.

   진입시, 폴트가 발생한 주소는 CR2 (컨트롤 레지스터 2)에 있으며,
   F의 error_code 멤버에 있는 폴트에 대한 정보는 PF_* 매크로에 설명된 형식으로
   포맷됩니다. 여기에 있는 예제 코드는 그 정보를 파싱하는 방법을 보여줍니다. */
static void page_fault(struct intr_frame *f) {
    bool not_present; /* 참: 페이지가 존재하지 않음, 거짓: 읽기 전용 페이지에
                         쓰기 시도. */
    bool write; /* 참: 쓰기 접근, 거짓: 읽기 접근. */
    bool user; /* 참: 사용자에 의한 접근, 거짓: 커널에 의한 접근. */
    void *fault_addr; /* 폴트 주소. */

    /* 폴트를 일으킨 주소를 얻습니다. 이는 가상 주소이며 코드나 데이터를 가리킬
       수 있습니다. 반드시 폴트를 일으킨 명령어의 주소는 아닙니다 (그것은 f->rip
       입니다). */
    fault_addr = (void *)rcr2();

    /* 인터럽트를 다시 켭니다 (CR2를 읽기 전에 변경되지 않도록 인터럽트를 끈
     * 것입니다). */
    intr_enable();

    /* 원인을 파악합니다. */
    not_present = (f->error_code & PF_P) == 0;
    write = (f->error_code & PF_W) != 0;
    user = (f->error_code & PF_U) != 0;

    exit(-1);

#ifdef VM
    /* 프로젝트 3 이후에 대한 부분. */
    if (vm_try_handle_fault(f, fault_addr, user, write, not_present)) return;
#endif

    /* 페이지 폴트 횟수를 셉니다. */
    page_fault_cnt++;

    /* 실제 폴트인 경우, 정보를 출력하고 종료합니다. */
    printf("Page fault at %p: %s error %s page in %s context.\n", fault_addr,
           not_present ? "not present" : "rights violation",
           write ? "writing" : "reading", user ? "user" : "kernel");
    kill(f);
}
