#include "userprog/process.h"

#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/gdt.h"
#include "userprog/syscall.h"
#include "userprog/tss.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup(void);
static bool load(const char *file_name, struct intr_frame *if_);
static void initd(void *f_name);
static void __do_fork(void *);
void argument_stack(char **parse, int count, void **rsp);
/* 일반 프로세스 초기화기(initd 및 기타 프로세스를 위한). */
static void process_init(void) { struct thread *current = thread_current(); }

/* 첫 번째 사용자 프로그램인 "initd"를 FILE_NAME에서 로드하여 시작합니다.
 * 새 스레드는 process_create_initd()가 반환되기 전에
 * 스케줄될 수 있으며 (심지어 종료될 수도 있음) initd의
 * 스레드 ID를 반환하거나, 스레드를 생성할 수 없는 경우 TID_ERROR를 반환합니다.
 * 이것은 한 번만 호출되어야 합니다. */
tid_t process_create_initd(const char *file_name) {
    char *fn_copy;
    tid_t tid;

    /* FILE_NAME의 복사본을 만듭니다.
     * 그렇지 않으면 호출자와 load() 간에 경쟁 상태가 발생합니다. */
    fn_copy = palloc_get_page(0);
    if (fn_copy == NULL) return TID_ERROR;
    strlcpy(fn_copy, file_name, PGSIZE);

    char *arg1;
    char *next_ptr;

    arg1 = strtok_r(file_name, " ", &next_ptr);
    struct thread *test = thread_current();
    /* FILE_NAME을 실행하기 위해 새 스레드를 생성합니다. */

    tid = thread_create(file_name, PRI_DEFAULT, initd, fn_copy);
    if (tid == TID_ERROR) palloc_free_page(fn_copy);
    return tid;
}

/* 첫 번째 사용자 프로세스를 실행하는 스레드 함수. */
static void initd(void *f_name) {
#ifdef VM
    supplemental_page_table_init(&thread_current()->spt);
#endif

    process_init();

    if (process_exec(f_name) < 0) PANIC("Fail to launch initd\n");
    NOT_REACHED();
}

/* `name`으로 현재 프로세스를 복제합니다. 새 프로세스의 스레드 ID를 반환하거나,
 * 스레드를 생성할 수 없는 경우 TID_ERROR를 반환합니다. */
tid_t process_fork(const char *name, struct intr_frame *if_ UNUSED) {
    /* 현재 스레드를 새 스레드로 복제합니다. */

    struct thread *parent = thread_current();

    // struct intr_frame *f = malloc(sizeof(struct intr_frame));
    // memcpy(f, if_, sizeof(struct intr_frame)); /*이거 복제 안해줘도
    // 되는데?*/
    tid_t tid = thread_create(name, PRI_DEFAULT, __do_fork, if_);
    if (tid == TID_ERROR) {
        return TID_ERROR;
    }
    // 각 자식들을 관리해야하기 때문에, child sema를 쓰는것이 합리적임.
    struct thread *child = get_child_process(tid);
    sema_down(&(child->load_sema));

    return tid;  // 자식프로세스 tid반환
}

#ifndef VM
/* 부모의 주소 공간을 복제하기 위해 pml4_for_each에 이 함수를 전달합니다.
 * 이것은 프로젝트 2에만 사용됩니다. */
static bool duplicate_pte(uint64_t *pte, void *va, void *aux) {
    struct thread *current = thread_current();
    struct thread *parent = (struct thread *)aux;
    void *parent_page;
    void *newpage;
    bool writable;

    /* 1. TODO: 부모 페이지가 커널 페이지인 경우 즉시 반환합니다. */
    if (is_kernel_vaddr(va)) {
        return true;  // true아니면 절대로 안됨!
    }

    /* 2. 부모의 페이지 맵 레벨 4에서 VA를 해석합니다. */
    parent_page = pml4_get_page(parent->pml4, va);

    /* 3. TODO: 자식을 위한 새 PAL_USER 페이지를 할당하고
     *    TODO: 결과를 NEWPAGE로 설정합니다. */
    newpage = palloc_get_page(PAL_USER | PAL_ZERO);
    if (newpage == NULL) {
        return false;
    }

    /* 4. TODO: 부모의 페이지를 새 페이지로 복제하고
     *    TODO: 부모 페이지가 쓰기 가능한지 확인하고 (WRITABLE에 결과를 설정) */

    memcpy(newpage, parent_page, PGSIZE);
    pte = pml4e_walk(parent->pml4, va, 0);
    writable = is_writable(pte);
    /* 5. 주소 VA에 WRITABLE 권한으로 자식의 페이지 테이블에 새 페이지를
     * 추가합니다. */
    if (!pml4_set_page(current->pml4, va, newpage, writable)) {
        /* 6. TODO: if fail to insert page, do error handling. */
        return false;
    }
    return true;
}
#endif

/* 부모의 실행 컨텍스트를 복사하는 스레드 함수입니다.
 * 힌트) parent->tf는 프로세스의 사용자 영역 컨텍스트를 유지하지 않습니다.
 *       즉, 이 함수에 process_fork의 두 번째 인수를 전달해야 합니다. */
static void __do_fork(void *aux) {
    struct intr_frame *parent_if = aux;
    struct intr_frame if_;
    struct thread *parent = thread_current()->parent;
    struct thread *current = thread_current();
    char *test_parent_name = parent->name;
    char *test_child_name = current->name;
    /* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */

    bool succ = true;

    /* 1. CPU 컨텍스트를 로컬 스택에 읽습니다. */
    memcpy(&if_, parent_if, sizeof(struct intr_frame));
    if_.R.rax = 0;
    /* 2. PT 복제 */
    current->pml4 = pml4_create();
    if (current->pml4 == NULL) goto error;

    process_activate(current);
#ifdef VM
    supplemental_page_table_init(&current->spt);
    if (!supplemental_page_table_copy(&current->spt, &parent->spt)) goto error;
#else
    if (!pml4_for_each(parent->pml4, duplicate_pte, parent)) goto error;
#endif

    /* TODO: 여기에 코드가 들어갑니다.
     * TODO: 힌트) 파일 객체를 복제하려면 include/filesys/file.h의
     * `file_duplicate`을 사용하세요. */
    struct file **current_fdt = current->fd_table;
    struct file **parent_fdt = parent->fd_table;
    struct file *new_file;
    for (int i = 2; i < 128; i++) {
        if (parent_fdt[i] != NULL) {
            new_file = file_duplicate(parent_fdt[i]);
            current_fdt[i] = new_file;
        }
    }
    current->next_fd_idx = parent->next_fd_idx;
    /*
    * TODO:       부모는 이 함수가 부모의 자원을 성공적으로 복제할 때까지

    * fork()에서 반환해서는 안 됩니다.*/
    sema_up(&(current->load_sema));
    process_init();  // 이함수는 왜 쓰이는건지 의문임 ㅋㅋ

    /* 마지막으로, 새로 생성된 프로세스로 전환합니다. */
    if (succ) {
        do_iret(&if_);
    }
error:
    sema_up(&(current->load_sema));
    sys_exit(TID_ERROR);
    // thread_exit();
}

/* f_name을 실행하는 컨텍스트로 현재 실행 컨텍스트를 전환합니다.
 * 실패하면 -1을 반환합니다. */
int process_exec(void *f_name) {
    // 현재 스레드의 스택프레임이 아니라, 커널에 안전하게 보관해야
    // 아래 process_cleanup()에서 사라지지 않습니다. 참고!!!!
    char *safe_name = (char *)palloc_get_page(PAL_ZERO);
    strlcpy(safe_name, (char *)f_name, strlen(f_name) + 1);
    bool success;
    char *cur_name = thread_current()->name;
    /* 우리는 현재 스레드 구조체에 있는 intr_frame을 사용할 수 없습니다.
     * 이는 현재 스레드가 재스케줄될 때 실행 정보를 멤버에 저장하기 때문입니다.
     */
    struct intr_frame _if;

    _if.ds = _if.es = _if.ss = SEL_UDSEG;
    _if.cs = SEL_UCSEG;
    _if.eflags = FLAG_IF | FLAG_MBS;

    /* 우리는 먼저 현재 컨텍스트를 종료합니다. */
    process_cleanup();

    /* 그리고 바이너리를 로드합니다. */
    char *arg_list[100];  // argument 배열
    int count = 0;        // argument 개수
    char *arg;
    char *rest;  // 분리된 문자열 중 남는 부분의 시작주소
    arg = strtok_r(safe_name, " ", &rest);
    arg_list[count++] = arg;
    while ((arg = strtok_r(NULL, " ", &rest))) {
        arg_list[count++] = arg;
    }

    arg_list[count] = NULL;

    /* 그리고 바이너리를 로드합니다. */

    success = load(safe_name, &_if);
    if (!success) {
        // exec-missing test case에서 이렇게 바꿔야 처리가 가능함.

        palloc_free_page(safe_name);
        return -1;
    }
    // 스택에 인자 넣기

    argument_stack(arg_list, count, &_if.rsp);
    _if.R.rdi = count;
    _if.R.rsi = (uint64_t)_if.rsp + 8;  // 이게맞나?

    // hex_dump(_if.rsp, _if.rsp, USER_STACK - _if.rsp, true);
    /* 로드에 실패하면 종료합니다. */
    palloc_free_page(safe_name);

    /* 스위칭된 프로세스를 시작합니다. */
    do_iret(&_if);
    NOT_REACHED();
}

void argument_stack(char **parse, int count, void **rsp) {
    int i, j;
    uint64_t *argv[count];

    // 인자 역순으로 스택에 푸시
    for (i = count - 1; i >= 0; i--) {
        for (j = strlen(parse[i]); j >= 0; j--) {
            *rsp = *rsp - 1;
            **(char **)rsp = parse[i][j];
        }
        argv[i] = (uint64_t *)*rsp;
    }

    // 워드 정렬을 위한 패딩 추가
    while ((uint64_t)*rsp % 8 != 0) {
        *rsp = *rsp - 1;
        **(uint8_t **)rsp = 0;
    }

    // NULL 포인터 센티넬 추가
    *rsp = *rsp - 8;
    **(uint64_t **)rsp = 0;

    // 인자 주소를 스택에 푸시
    for (i = count - 1; i >= 0; i--) {
        *rsp = *rsp - 8;
        **(uint64_t **)rsp = (uint64_t)argv[i];
    }

    // 페이크 리턴 주소 푸시
    *rsp = *rsp - 8;
    **(uint64_t **)rsp = 0;
}

/* 스레드 TID가 종료될 때까지 기다렸다가 그것의 종료 상태를 반환합니다. 만약
 * 커널에 의해 종료되었다면(즉, 예외로 인해 종료되었다면) -1을 반환합니다. 만약
 * TID가 유효하지 않거나 호출 프로세스의 자식이 아니거나, 이미 해당 TID에 대해
 * process_wait()가 성공적으로 호출되었다면, 기다리지 않고 즉시 -1을 반환합니다.
 *
 * 이 함수는 문제 2-2에서 구현될 것입니다. 지금은 아무 것도 하지 않습니다. */
int process_wait(tid_t child_tid) {
    struct thread *child = get_child_process(child_tid);

    // 해당하는 tid의 자식 프로세스가 없는 경우 -1 return
    if (child == NULL) {
        return -1;
    }

    sema_down(&child->wait_sema);
    list_remove(&child->child_elem);
    sema_up(&child->exit_sema);

    return child->exit_status;
}

void process_close_file(int fd) {
    /*파일닫기*/
    struct thread *cur = thread_current();
    struct file **cur_fdt = cur->fd_table;
    if (cur_fdt[fd] == NULL) {
        return;
    }
    file_close(cur_fdt[fd]);
    cur_fdt[fd] = NULL;
}

/* 프로세스를 종료합니다. 이 함수는 thread_exit()에 의해 호출됩니다. */
void process_exit(void) {
    struct thread *curr = thread_current();
    /* TODO: 여기에 코드가 들어갑니다.
     * TODO: 프로세스 종료 메시지 구현 (project2/process_termination.html 참조).
     * TODO: 프로세스 리소스 정리를 여기에서 구현하는 것이 좋습니다. */
    for (int i = 2; i < 128; i++) {
        process_close_file(i);
    }
    // palloc_free_page(curr->fd_table);
    palloc_free_multiple(curr->fd_table, 4);
    file_close(curr->exec_file);
    process_cleanup();
    sema_up(&curr->wait_sema);
    sema_down(&curr->exit_sema);
}

struct thread *get_child_process(int pid) {
    struct thread *curr = thread_current();
    struct list_elem *e;

    for (e = list_begin(&curr->child_list); e != list_end(&curr->child_list);
         e = list_next(e)) {
        struct thread *t = list_entry(e, struct thread, child_elem);
        if (pid == t->tid) {
            return t;
        }
    }
    return NULL;
}

static void start_process(void *f_name) {
    process_init();
    process_exec(f_name);
    NOT_REACHED();
}

/* 현재 프로세스의 리소스를 해제합니다. */
static void process_cleanup(void) {
    struct thread *curr = thread_current();

#ifdef VM
    supplemental_page_table_kill(&curr->spt);
#endif

    uint64_t *pml4;
    /* 현재 프로세스의 페이지 디렉토리를 파괴하고 커널 전용 페이지 디렉토리로
     * 전환합니다. */
    pml4 = curr->pml4;
    if (pml4 != NULL) {
        /* 여기서의 순서가 중요합니다. 페이지 디렉토리를 전환하기 전에
         * curr->pml4를 NULL로 설정해야 합니다. 그래야 타이머 인터럽트가
         * 프로세스 페이지 디렉토리로 다시 전환하는 것을 방지할 수 있습니다.
         * 프로세스의 페이지 디렉토리를 파괴하기 전에 기본 페이지 디렉토리를
         * 활성화해야 합니다. 그렇지 않으면 활성화된 페이지 디렉토리가
         * 해제되어 지워진 상태가 됩니다. */
        curr->pml4 = NULL;
        pml4_activate(NULL);
        pml4_destroy(pml4);
    }
}

/* 다음 스레드에서 사용자 코드를 실행하기 위해 CPU를 설정합니다.
 * 이 함수는 모든 컨텍스트 전환에서 호출됩니다. */
void process_activate(struct thread *next) {
    /* 스레드의 페이지 테이블을 활성화합니다. */
    pml4_activate(next->pml4);

    /* 인터럽트 처리에 사용될 스레드의 커널 스택을 설정합니다. */
    tss_update(next);
}

/* ELF 바이너리를 로드합니다. 다음 정의는 ELF 사양에서 가져온 것입니다. */

/* ELF 타입. [ELF1] 1-2 참조. */
#define EI_NIDENT 16

#define PT_NULL 0           /* 무시. */
#define PT_LOAD 1           /* 로드 가능한 세그먼트. */
#define PT_DYNAMIC 2        /* 동적 링킹 정보. */
#define PT_INTERP 3         /* 동적 로더의 이름. */
#define PT_NOTE 4           /* 보조 정보. */
#define PT_SHLIB 5          /* 예약됨. */
#define PT_PHDR 6           /* 프로그램 헤더 테이블. */
#define PT_STACK 0x6474e551 /* 스택 세그먼트. */

#define PF_X 1 /* 실행 가능. */
#define PF_W 2 /* 쓰기 가능. */
#define PF_R 4 /* 읽기 가능. */

/* 실행 가능한 헤더. [ELF1] 1-4부터 1-8까지.
 * 이것은 ELF 바이너리의 맨 처음에 나타납니다. */

struct ELF64_hdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct ELF64_PHDR {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

/* 축약어 */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack(struct intr_frame *if_);
static bool validate_segment(const struct Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable);

/* FILE_NAME에서 ELF 실행 파일을 현재 스레드로 로드합니다.
 * 실행 파일의 진입점을 *RIP에 저장하고
 * 초기 스택 포인터를 *RSP에 저장합니다.
 * 성공하면 true를 반환하고, 그렇지 않으면 false를 반환합니다. */
#define MAX_ARGS 128
static bool load(const char *file_name, struct intr_frame *if_) {
    struct thread *t = thread_current();
    struct ELF ehdr;
    struct file *file = NULL;
    off_t file_ofs;
    bool success = false;
    int i;

    /* 페이지 디렉토리 할당 및 활성화 */
    t->pml4 = pml4_create();
    if (t->pml4 == NULL) goto done;
    process_activate(thread_current());

    /* 실행 파일 열기 */
    file = filesys_open(file_name);
    if (file == NULL) {
        printf("load: %s: open failed\n", file_name);
        goto done;
    }

    t->exec_file = file;
    file_deny_write(file);

    /* 실행 가능한 헤더 읽기 및 검증 */
    if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
        memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7) || ehdr.e_type != 2 ||
        ehdr.e_machine != 0x3E  // amd64
        || ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Phdr) ||
        ehdr.e_phnum > 1024) {
        printf("load: %s: error loading executable\n", file_name);
        goto done;
    }

    /* 프로그램 헤더 읽기 */
    file_ofs = ehdr.e_phoff;
    for (i = 0; i < ehdr.e_phnum; i++) {
        struct Phdr phdr;

        if (file_ofs < 0 || file_ofs > file_length(file)) goto done;
        file_seek(file, file_ofs);

        if (file_read(file, &phdr, sizeof phdr) != sizeof phdr) goto done;
        file_ofs += sizeof phdr;
        switch (phdr.p_type) {
            case PT_NULL:
            case PT_NOTE:
            case PT_PHDR:
            case PT_STACK:
            default:
                /* 이 세그먼트는 무시합니다. */
                break;
            case PT_DYNAMIC:
            case PT_INTERP:
            case PT_SHLIB:
                goto done;
            case PT_LOAD:
                if (validate_segment(&phdr, file)) {
                    bool writable = (phdr.p_flags & PF_W) != 0;
                    uint64_t file_page = phdr.p_offset & ~PGMASK;
                    uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
                    uint64_t page_offset = phdr.p_vaddr & PGMASK;
                    uint32_t read_bytes, zero_bytes;
                    if (phdr.p_filesz > 0) {
                        /* 일반 세그먼트.
                         * 디스크에서 초기 부분을 읽고 나머지는 0으로 채웁니다.
                         */
                        read_bytes = page_offset + phdr.p_filesz;
                        zero_bytes =
                            (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) -
                             read_bytes);
                    } else {
                        /* 완전히 0.
                         * 디스크에서 아무것도 읽지 않습니다. */
                        read_bytes = 0;
                        zero_bytes =
                            ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
                    }
                    if (!load_segment(file, file_page, (void *)mem_page,
                                      read_bytes, zero_bytes, writable))
                        goto done;
                } else
                    goto done;
                break;
        }
    }

    t->exec_file = file;
    file_deny_write(file);
    /* 스택 초기화 */
    if (!setup_stack(if_)) goto done;

    /* 시작 주소 설정 */
    if_->rip = ehdr.e_entry;

    /* TODO: 여기에 코드 추가.
     * TODO: 인자 전달 구현 (project2/argument_passing.html 참조). */

    success = true;

done:

    /* We arrive here whether the load is successful or not. */
    // file_close(file);
    return success;
}

/* PHDR가 파일 내에서 유효하고 로드 가능한 세그먼트를 설명하는지 확인하고,
 * 그렇다면 true를 반환 */
static bool validate_segment(const struct Phdr *phdr, struct file *file) {
    /* p_offset과 p_vaddr는 같은 페이지 오프셋을 가져야 함 */
    if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) return false;

    /* p_offset은 파일 내부를 가리켜야 함 */
    if (phdr->p_offset > (uint64_t)file_length(file)) return false;

    /* p_memsz는 p_filesz보다 크거나 같아야 함 */
    if (phdr->p_memsz < phdr->p_filesz) return false;

    /* 세그먼트는 비어 있지 않아야 함 */
    if (phdr->p_memsz == 0) return false;

    /* 가상 메모리 영역은 사용자 주소 공간 범위 내에서 시작하고 끝나야 함 */
    if (!is_user_vaddr((void *)phdr->p_vaddr)) return false;
    if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz))) return false;

    /* 영역은 커널 가상 주소 공간을 걸쳐 "wrap around"되어서는 안 됨 */
    if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr) return false;

    /* 페이지 0 매핑 금지.
       페이지 0을 매핑하는 것은 나쁜 생각이며, 허용한다면 사용자 코드가 시스템
       호출에 null 포인터를 전달하여 memcpy() 등에서 null 포인터 단언문을 통해
       커널을 패닉시킬 수 있음 */
    if (phdr->p_vaddr < PGSIZE) return false;

    /* It's okay. */
    return true;
}

#ifndef VM
/* 이 블록의 코드는 project 2 동안에만 사용됩니다.
 * 전체 project 2에 대한 함수를 구현하려면 #ifndef 매크로 바깥에 구현하세요. */

/* load() 헬퍼 함수들 */
static bool install_page(void *upage, void *kpage, bool writable);

/* 파일의 OFS 오프셋에서 시작하여 UPAGE 주소에 세그먼트를 로드합니다.
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리가 초기화됩니다.
 * - UPAGE에서 READ_BYTES 바이트는 OFS 오프셋에서 시작하는 파일에서 읽어야
 * 합니다.
 * - UPAGE + READ_BYTES에서 ZERO_BYTES 바이트는 0으로 초기화되어야 합니다.
 * 이 함수에 의해 초기화된 페이지는 WRITABLE이 true이면 사용자 프로세스가 쓰기
 * 가능해야 하며, 그렇지 않으면 읽기 전용이어야 합니다. 성공하면 true를
 * 반환하고, 메모리 할당 오류 또는 디스크 읽기 오류가 발생하면 false를
 * 반환합니다. */
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable) {
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);

    file_seek(file, ofs);
    while (read_bytes > 0 || zero_bytes > 0) {
        /* 이 페이지를 채우는 방법을 계산합니다.
         * 파일에서 PAGE_READ_BYTES 바이트를 읽고
         * 마지막 PAGE_ZERO_BYTES 바이트를 0으로 채웁니다. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* 메모리 페이지를 얻습니다. */
        uint8_t *kpage = palloc_get_page(PAL_USER);
        if (kpage == NULL) return false;

        /* 이 페이지를 로드합니다. */
        if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
            palloc_free_page(kpage);
            return false;
        }
        memset(kpage + page_read_bytes, 0, page_zero_bytes);

        /* 페이지를 프로세스의 주소 공간에 추가합니다. */
        if (!install_page(upage, kpage, writable)) {
            printf("fail\n");
            palloc_free_page(kpage);
            return false;
        }

        /* 진행합니다. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }
    return true;
}

/* USER_STACK에 제로화된 페이지를 매핑하여 최소 스택을 생성합니다 */
static bool setup_stack(struct intr_frame *if_) {
    uint8_t *kpage;
    bool success = false;

    kpage = palloc_get_page(PAL_USER | PAL_ZERO);
    if (kpage != NULL) {
        success = install_page(((uint8_t *)USER_STACK) - PGSIZE, kpage, true);
        if (success)
            if_->rsp = USER_STACK;
        else
            palloc_free_page(kpage);
    }
    return success;
}

/* 사용자 가상 주소 UPAGE에 커널 가상 주소 KPAGE를 매핑합니다.
 * WRITABLE이 true이면 사용자 프로세스가 페이지를 수정할 수 있습니다; 그렇지
 * 않으면 읽기 전용입니다. UPAGE는 이미 매핑되어 있지 않아야 합니다. KPAGE는
 * palloc_get_page()로 얻은 사용자 풀의 페이지여야 합니다. 성공하면 true를
 * 반환하고, UPAGE가 이미 매핑되어 있거나 메모리 할당이 실패하면 false를
 * 반환합니다. */
static bool install_page(void *upage, void *kpage, bool writable) {
    struct thread *t = thread_current();

    /* 해당 가상 주소에 이미 페이지가 있는지 확인한 후,
     * 우리의 페이지를 그곳에 매핑합니다. */
    return (pml4_get_page(t->pml4, upage) == NULL &&
            pml4_set_page(t->pml4, upage, kpage, writable));
}
#else
/* 여기서부터, 코드는 project 3 이후에 사용됩니다.
 * project 2에 대해서만 함수를 구현하려면 위의 블록에 구현하세요. */

static bool lazy_load_segment(struct page *page, void *aux) {
    /* TODO: Load the segment from the file */
    /* TODO: This called when the first page fault occurs on address VA. */
    /* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable) {
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);

    while (read_bytes > 0 || zero_bytes > 0) {
        /* Do calculate how to fill this page.
         * We will read PAGE_READ_BYTES bytes from FILE
         * and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* TODO: Set up aux to pass information to the lazy_load_segment. */
        void *aux = NULL;
        if (!vm_alloc_page_with_initializer(VM_ANON, upage, writable,
                                            lazy_load_segment, aux))
            return false;

        /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }
    return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool setup_stack(struct intr_frame *if_) {
    bool success = false;
    void *stack_bottom = (void *)(((uint8_t *)USER_STACK) - PGSIZE);

    /* TODO: Map the stack on stack_bottom and claim the page immediately.
     * TODO: If success, set the rsp accordingly.
     * TODO: You should mark the page is stack. */
    /* TODO: Your code goes here */

    return success;
}
#endif /* VM */