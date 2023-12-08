#include "userprog/syscall.h"

#include <stdio.h>
#include <syscall-nr.h>

#include "filesys/filesys.h"
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

void check_address(void *addr) {
    if (!is_user_vaddr(addr)) {
        exit(-1);
    }
}

struct thread *get_child_process(int tid) {
    /* 자식 리스트를 검색하여 프로세스 디스크립터의 주소 리턴 */
    struct list *cur_child_list = &thread_current()->child_list;
    struct list_elem *child;
    struct thread *t;
    for (child = list_begin(cur_child_list);
         child != list_end(cur_child_list);) {
        t = list_entry(child, struct thread, child_elem);
        if (t->tid == tid) {
            return t;
        }
        child = list_next(child);
    }
    return NULL;
}

void remove_child_process(struct thread *cp) {
    /*프로세스 디스크립터를 자식 리스트에서 제거 후 메모리 해제*/
    struct list *cur_child_list = &thread_current()->child_list;
    struct list_elem *child;
    struct thread *t;
    for (child = list_begin(cur_child_list);
         child != list_end(cur_child_list);) {
        t = list_entry(child, struct thread, child_elem);
        if (t == cp) {
            list_remove(child);
            palloc_free_page(t);  // 이게 맞는지 모르겠따.
        }
        child = list_next(child);
    }
    // 메모리해제는 어떻게해야하노??
}

void halt(void) { power_off(); }

void exit(int status) {
    printf("%s: exit(%d)\n", thread_current()->name, status);
    thread_exit();
}

bool create(const char *file, unsigned initial_size) {
    if (file == NULL) exit(-1);
    bool result = (filesys_create(file, initial_size));
    return result;
}

bool remove(const char *file) {
    if (file == NULL) exit(-1);
    bool result = (filesys_remove(file));
    return result;
}

int write(int fd, void *buffer, unsigned size) {
    check_address(buffer);
    if (fd == 1) {
        putbuf(buffer, size);
        return size;
    } else {
        return -1;
    }
}

// for open syscall!
int process_add_file(struct file *f) {
    struct thread *cur = thread_current();
    struct file **cur_fdt = cur->fdt;
    int file_descriptor = cur->next_fd;
    cur_fdt[file_descriptor] = f;
    cur->next_fd++;

    return file_descriptor;
}
int open(const char *file) {
    if (file == NULL) exit(-1);
    struct file *open_file = filesys_open(file);
    if (open_file == NULL) {
        return -1;
    }
    int fd = process_add_file(open_file);
    return fd;
}


void syscall_handler(struct intr_frame *f) {
    // 시스템 콜 번호를 RAX 레지스터로부터 읽어옵니다.

    int syscall_number = f->R.rax;

    unsigned initial_size;
    const char *file;
    // 시스템 콜 번호에 따라 적절한 처리 수행
    switch (syscall_number) {
        case SYS_HALT:
            halt();
            break;
        case SYS_EXIT:
            exit((int)f->R.rdi);
            break;
        case SYS_FORK:
            break;
        case SYS_EXEC:
            break;
        case SYS_WAIT:
            break;
        case SYS_CREATE:
            check_address(f->R.rdi);
            f->R.rax = create(f->R.rdi, f->R.rsi);
            break;
        case SYS_REMOVE:
            check_address(f->R.rdi);
            f->R.rax = remove(f->R.rdi);
            break;
        case SYS_OPEN:
            check_address(f->R.rdi);
            f->R.rax = open(f->R.rdi);
            break;
        case SYS_FILESIZE:
            break;
        case SYS_READ:
            break;
        case SYS_WRITE:
            f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
            // printf("여기로 튀나?\n");
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
    // f->R.rax = syscall_result;

    // 시스템 콜이 종료된 후의 동작을 수행할 수 있습니다.
    // 예를 들어, 스레드를 종료시키는 대신 다른 작업을 수행할 수 있습니다.
    // thread_exit();
}