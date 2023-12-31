#include "userprog/syscall.h"

#include <stdio.h>
#include <syscall-nr.h>

#include "filesys/file.h"
#include "filesys/filesys.h"
#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/loader.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "userprog/gdt.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
void syscall_entry(void);
void syscall_handler(struct intr_frame *);
struct file *process_get_file(int fd);
typedef int pid_t;
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
    lock_init(&filesys_lock);
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
        sys_exit(-1);
    }
}

void sys_halt(void) { power_off(); }

void sys_exit(int status) {
    thread_current()->exit_status = status;
    printf("%s: exit(%d)\n", thread_current()->name, status);
    thread_exit();
}

pid_t sys_fork(const char *thread_name, struct intr_frame *if_) {
    return process_fork(thread_name, if_);
}

/*
요구사항:
성공적으로 진행된다면 어떤 것도 반환하지 않습니다.
만약 프로그램이 이 프로세스를 로드하지 못하거나
다른 이유로 돌리지 못하게 되면 exit state -1을 반환하며 프로세스가 종료됩니다.*/
int sys_exec(const char *file) {
    if (file == NULL) sys_exit(-1);
    if (process_exec((void *)file) < 0) sys_exit(-1);
}

bool sys_create(const char *file, unsigned initial_size) {
    if (file == NULL) sys_exit(-1);
    // lock_acquire(&filesys_lock);
    bool result = (filesys_create(file, initial_size));
    // lock_release(&filesys_lock);
    return result;
}

bool sys_remove(const char *file) {
    if (file == NULL) sys_exit(-1);
    // lock_acquire(&filesys_lock);
    bool result = (filesys_remove(file));
    // lock_release(&filesys_lock);
    return result;
}

int sys_open(const char *file) {
    if (file == NULL) sys_exit(-1);

    struct file *f = filesys_open(file);

    if (f == NULL) {
        return -1;
    }
    if (thread_current()->next_fd_idx >= 128) {
        file_close(f);
        return -1;
    }

    thread_current()->fd_table[thread_current()->next_fd_idx] = f;
    return thread_current()->next_fd_idx++;
}

int sys_filesize(int fd) {
    if (fd < 0 || fd >= thread_current()->next_fd_idx) {
        return -1;
    }
    struct file *f = thread_current()->fd_table[fd];
    if (f == NULL) {
        return -1;
    }
    return file_length(f);
}

void sys_close(int fd) {
    if (fd < 0 || fd >= thread_current()->next_fd_idx) {
        return;
    }
    struct file *f = thread_current()->fd_table[fd];
    if (f == NULL) {
        return;
    }
    file_close(f);
    thread_current()->fd_table[fd] = NULL;
}

int sys_read(int fd, void *buffer, unsigned size) {
    if (fd == 0) {
        int cnt = 0;
        while (1) {             // size만큼 받았거나, '\n'이 오면 끝
            if (cnt >= size) {  // size만큼 받은 경우
                break;
            }
            cnt++;

            char key = input_getc();
            if (key == '\n') {  // '\n'이 온 경우
                char *buffer = key;
                break;
            }

            char *buffer = key;
            buffer++;
        }
    } else if (2 <= fd && fd < 128) {
        struct file *curr_file = thread_current()->fd_table[fd];
        if (curr_file == NULL) return -1;

        lock_acquire(&filesys_lock);  // lock 걸기
        off_t read_result = file_read(curr_file, buffer, size);
        lock_release(&filesys_lock);  // lock 풀기
        return read_result;
    } else {
        return -1;
    }
}

int sys_write(int fd, const void *buffer, unsigned size) {
    if (fd == 1) {
        putbuf(buffer, size);
        return size;

    } else if (2 <= fd && fd < 128) {
        struct file *curr_file = thread_current()->fd_table[fd];
        if (curr_file == NULL) return -1;
        lock_acquire(&filesys_lock);  // lock 걸기
        off_t write_result = file_write(curr_file, buffer, size);
        lock_release(&filesys_lock);  // lock 풀기
        return write_result;
    } else {
        return -1;
    }
}

/* 열린 파일의 위치(offset)를 이동하는 시스템 콜
    Position : 현재 위치(offset)를 기준으로 이동할 거리 */

void sys_seek(int fd, unsigned position) {
    struct file *cur_file = process_get_file(fd);

    if (cur_file == NULL) {
        return;
    }

    file_seek(cur_file, position);
}

/* 열린 파일의 위치(offset)를 알려주는 시스템 콜
   성공 시 파일의 위치(offset)를 반환, 실패 시 -1 반환 */
unsigned sys_tell(int fd) {
    if (fd < 0 || fd >= thread_current()->next_fd_idx) {
        return -1;
    }

    struct file *cur_file = process_get_file(fd);
    if (cur_file == NULL) {
        return -1;
    }
    return file_tell(cur_file);
}

/* 파일 객체(struct file)를 검색하는 함수 */
struct file *process_get_file(int fd) {
    struct thread *cur = thread_current();
    if (fd < 0 || fd >= cur->next_fd_idx) {
        return NULL;
    }

    struct file **cur_fdt = cur->fd_table;
    if (cur_fdt[fd] == NULL) {
        return NULL;
    }
    return cur_fdt[fd];
}

int sys_wait(int pid) { return process_wait(pid); }

void syscall_handler(struct intr_frame *f) {
    // 시스템 콜 번호를 RAX 레지스터로부터 읽어옵니다.

    int syscall_number = f->R.rax;

    unsigned initial_size;
    const char *file;
    // 시스템 콜 번호에 따라 적절한 처리 수행
    switch (syscall_number) {
        case SYS_HALT:
            sys_halt();
            break;
        case SYS_EXIT:
            sys_exit((int)f->R.rdi);
            break;
        case SYS_FORK:
            check_address(f->R.rdi);
            f->R.rax = sys_fork(f->R.rdi, f);
            break;
        case SYS_EXEC:
            check_address(f->R.rdi);
            f->R.rax = sys_exec(f->R.rdi);
            break;
        case SYS_WAIT:
            check_address(f->R.rdi);
            f->R.rax = sys_wait(f->R.rdi);
            break;
        case SYS_CREATE:
            check_address(f->R.rdi);
            f->R.rax = sys_create(f->R.rdi, f->R.rsi);
            break;
        case SYS_REMOVE:
            check_address(f->R.rdi);
            f->R.rax = sys_remove(f->R.rdi);
            break;
        case SYS_OPEN:
            check_address(f->R.rdi);
            f->R.rax = sys_open(f->R.rdi);
            break;
        case SYS_FILESIZE:
            f->R.rax = sys_filesize(f->R.rdi);
            break;
        case SYS_READ:
            f->R.rax = sys_read(f->R.rdi, f->R.rsi, f->R.rdx);
            break;
        case SYS_WRITE:
            f->R.rax = sys_write(f->R.rdi, f->R.rsi, f->R.rdx);
            break;
        case SYS_SEEK:
            sys_seek(f->R.rdi, f->R.rsi);
            break;
        case SYS_TELL:
            f->R.rax = sys_tell(f->R.rdi);
            break;
        case SYS_CLOSE:
            sys_close(f->R.rdi);
            break;

        default:
            sys_exit(-1);
            break;
    }
}