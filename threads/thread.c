#include "threads/thread.h"

#include <debug.h>
#include <random.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/fixed_point.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* struct thread의 `magic` 멤버에 대한 임의의 값.
   스택 오버플로우를 감지하기 위해 사용됩니다. 자세한 내용은 thread.h 상단의 큰 주석을 참조하세요. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본 스레드에 대한 임의의 값
   이 값을 수정하지 마세요. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태인 프로세스 목록, 즉 실행 준비가 되었으나 실제로 실행 중이지 않은 프로세스들입니다. */
static struct list ready_list;
static struct list sleep_list;

/* 다음 깨워야 할 최소 시간 */
static int64_t min_wake_ticks;

/* 유휴 스레드. */
static struct thread *idle_thread;

/* 초기 스레드, init.c:main()을 실행하는 스레드. */
static struct thread *initial_thread;

/* allocate_tid()에서 사용되는 락. */
static struct lock tid_lock;

/* 스레드 파괴 요청 */
static struct list destruction_req;

/* 통계. */
static long long idle_ticks;   /* 유휴 상태에서 보낸 타이머 틱의 수. */
static long long kernel_ticks; /* 커널 스레드에서 보낸 타이머 틱의 수. */
static long long user_ticks;   /* 사용자 프로그램에서 보낸 타이머 틱의 수. */

/* 스케줄링. */
#define TIME_SLICE 4          /* 각 스레드에게 주어지는 타이머 틱의 수. */
static unsigned thread_ticks; /* 마지막 yield 이후의 타이머 틱 수. */

////////////추가됨
#define NICE_DEFAULT 0
#define RECENT_CPU_DEFAULT 0
#define LOAD_AVG_DEFAULT 0
int load_avg;

///////////추가됨



/* false (기본값)인 경우, 라운드-로빈 스케줄러를 사용합니다.
   true인 경우, 다중 레벨 피드백 큐 스케줄러를 사용합니다.
   커널 명령줄 옵션 "-o mlfqs"에 의해 제어됩니다. */
bool thread_mlfqs;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule(void);
static tid_t allocate_tid(void);
void thread_sleep(int64_t ticks);
void thread_wakeup(int64_t ticks);
void thread_switching(void);
/* T가 유효한 스레드를 가리키는 경우 true를 반환합니다. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 실행 중인 스레드를 반환합니다.
 * CPU의 스택 포인터 `rsp`를 읽고, 페이지 시작 부분으로 반올림합니다.
   `struct thread`는 항상 페이지의 시작 부분에 있고 스택 포인터는 중간 어딘가에 있으므로,
   이를 통해 현재 스레드를 찾을 수 있습니다. */
#define running_thread() ((struct thread *)(pg_round_down(rrsp())))

// 스레드 시작을 위한 전역 디스크립터 테이블.
// 스레드 초기화 후에 gdt가 설정될 것이므로, 우선 임시 gdt를 설정해야 합니다.
static uint64_t gdt[3] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff};

/* 스레딩 시스템을 초기화하여 현재 실행 중인 코드를 스레드로 변환합니다.
   이것은 일반적으로 작동하지 않으며, loader.S가 스택의 하단을 페이지 경계에 놓았기 때문에
   이 경우에만 가능합니다.

   또한 실행 대기 큐와 tid 락을 초기화합니다.

   이 함수를 호출한 후, thread_create()로 새 스레드를 생성하기 전에
   페이지 할당자를 초기화해야 합니다.

   이 함수가 완료될 때까지 thread_current()를 호출하는 것은 안전하지 않습니다. */
void thread_init(void) {
    ASSERT(intr_get_level() == INTR_OFF);

    /* 커널을 위한 임시 gdt를 다시 로드합니다.
     * 이 gdt는 사용자 컨텍스트를 포함하지 않습니다.
     * 커널은 gdt_init()에서 사용자 컨텍스트와 함께 gdt를 재구축할 것입니다. */
    struct desc_ptr gdt_ds = {
        .size = sizeof(gdt) - 1,
        .address = (uint64_t)gdt};
    lgdt(&gdt_ds);

    /* 전역 스레드 컨텍스트를 초기화합니다. */
    lock_init(&tid_lock);
    list_init(&ready_list);
    list_init(&sleep_list);
    list_init(&destruction_req);
	
    /* 실행 중인 스레드에 대한 스레드 구조체를 설정합니다. */
    initial_thread = running_thread();
    init_thread(initial_thread, "main", PRI_DEFAULT);
    initial_thread->status = THREAD_RUNNING;
    initial_thread->tid = allocate_tid();
    min_wake_ticks = INT64_MAX;
}

/* 선점 스레드 스케줄링을 시작하고 인터럽트를 활성화합니다.
   또한 유휴 스레드를 생성합니다. */
void thread_start(void) {
    /* 유휴 스레드를 생성합니다. */
    struct semaphore idle_started;
    sema_init(&idle_started, 0);
    thread_create("idle", PRI_MIN, idle, &idle_started);
    load_avg = LOAD_AVG_DEFAULT;
    /* 선점 스레드 스케줄링을 시작합니다. */
    intr_enable();

    /* 유휴 스레드가 idle_thread를 초기화할 때까지 기다립니다. */
    sema_down(&idle_started);
}

/* 타이머 인터럽트 핸들러가 각 타이머 틱마다 호출합니다.
   따라서, 이 함수는 외부 인터럽트 컨텍스트에서 실행됩니다. */
void thread_tick(void) {
    struct thread *t = thread_current();

    /* Update statistics. */
    if (t == idle_thread)
        idle_ticks++;
#ifdef USERPROG
    else if (t->pml4 != NULL)
        user_ticks++;
#endif
    else
        kernel_ticks++;

    /* Enforce preemption. */
    if (++thread_ticks >= TIME_SLICE)
        intr_yield_on_return();
}

/* 스레드 통계를 출력합니다. */
void thread_print_stats(void) {
    printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
           idle_ticks, kernel_ticks, user_ticks);
}

/* NAME 이름과 주어진 초기 PRIORITY를 가진 새로운 커널 스레드를 생성하고,
   FUNCTION을 AUX 인수로 실행하며, 준비 큐에 추가합니다.
   새 스레드의 스레드 식별자를 반환하거나, 생성에 실패하면 TID_ERROR를 반환합니다.

   thread_start()가 호출되었다면, 새 스레드는 thread_create()가 반환되기 전에
   스케줄될 수 있습니다. 심지어 thread_create()가 반환되기 전에 종료될 수도 있습니다.
   반대로, 원래 스레드는 새 스레드가 스케줄될 때까지 얼마든지 실행될 수 있습니다.
   순서를 보장하려면 세마포어 또는 동기화의 다른 형태를 사용하세요.

   제공된 코드는 새 스레드의 'priority' 멤버를 PRIORITY로 설정하지만,
   실제 우선순위 스케줄링은 구현되지 않았습니다.
   우선순위 스케줄링은 문제 1-3의 목표입니다. */
tid_t thread_create(const char *name, int priority,
                    thread_func *function, void *aux) {
    struct thread *t;
    tid_t tid;

    ASSERT(function != NULL);

    /* 스레드 할당. */
    t = palloc_get_page(PAL_ZERO);
    if (t == NULL)
        return TID_ERROR;

    /* 스레드 초기화. */
    init_thread(t, name, priority);
    tid = t->tid = allocate_tid();

    /* 스케줄된 경우 kernel_thread를 호출합니다.
     * 참고) rdi는 첫 번째 인자이고, rsi는 두 번째 인자입니다. */
    t->tf.rip = (uintptr_t)kernel_thread;
    t->tf.R.rdi = (uint64_t)function;
    t->tf.R.rsi = (uint64_t)aux;
    t->tf.ds = SEL_KDSEG;
    t->tf.es = SEL_KDSEG;
    t->tf.ss = SEL_KDSEG;
    t->tf.cs = SEL_KCSEG;
    t->tf.eflags = FLAG_IF;

    /* 실행 대기 큐에 추가. */
    thread_unblock(t);
    thread_switching();

    return tid;
}

/* 현재 수행중인 스레드와 가장 높은 우선순위의 스레드의 우선순위를 비교하여 스케줄링 */
void thread_switch(void) {
    if (!list_empty(&ready_list)) {
        struct list_elem *e = list_begin(&sleep_list);
        struct thread *ready_thread = list_entry(e, struct thread, elem);
        if (thread_get_priority() < ready_thread->priority) {
            thread_yield();
        }
    }
}

/* 현재 스레드를 대기 상태로 만듭니다. 다시 스케줄될 때까지 실행되지 않습니다.
   thread_unblock()에 의해 깨어날 때까지입니다.

   이 함수는 인터럽트가 꺼진 상태에서 호출되어야 합니다.
   일반적으로 synch.h의 동기화 원시에 사용하는 것이 더 좋은 생각입니다. */
void thread_block(void) {
    ASSERT(!intr_context());
    ASSERT(intr_get_level() == INTR_OFF);
    thread_current()->status = THREAD_BLOCKED;
    schedule();
}

/* 차단된 스레드 T를 실행 준비 상태로 전환합니다.
   이것은 T가 차단되지 않은 경우에는 오류입니다. (실행 중인 스레드를 준비 상태로 만들려면 thread_yield()를 사용하세요.)

   이 함수는 실행 중인 스레드를 선점하지 않습니다. 이것은 중요할 수 있습니다:
   호출자가 스스로 인터럽트를 비활성화한 경우, 스레드를 원자적으로 차단 해제하고
   다른 데이터를 업데이트할 수 있기를 기대할 수 있습니다. */
// 리스트에 삽입될 때 우선순위를 비교하는 함수

bool cmp_priority(const struct list_elem *a, const struct list_elem *b, void *aux) {
    struct thread *thread_a = list_entry(a, struct thread, elem);
    struct thread *thread_b = list_entry(b, struct thread, elem);

    return thread_a->priority > thread_b->priority;
}

bool cmp_wake_up_time(const struct list_elem *a, const struct list_elem *b, void *aux) {
    struct thread *thread_a = list_entry(a, struct thread, elem);
    struct thread *thread_b = list_entry(b, struct thread, elem);

    return thread_a->wake_up_time < thread_b->wake_up_time;
}

// 레디로 만드는 함수
void thread_unblock(struct thread *t) {
    enum intr_level old_level;

    ASSERT(is_thread(t));

    old_level = intr_disable();
    ASSERT(t->status == THREAD_BLOCKED);
    list_insert_ordered(&ready_list, &t->elem, cmp_priority, NULL);
    t->status = THREAD_READY;

    intr_set_level(old_level);
}

/* 실행 중인 스레드의 이름을 반환합니다. */
const char *
thread_name(void) {
    return thread_current()->name;
}

/* 실행 중인 스레드를 반환합니다.
   이것은 running_thread()와 몇 가지 적절한 검사입니다.
   자세한 내용은 thread.h 상단의 큰 주석을 참조하세요. */
struct thread *
thread_current(void) {
    struct thread *t = running_thread();

    /* T가 실제로 스레드인지 확인합니다.
       이 중 어느 하나라도 주장이 실패하면, 스레드의 스택이 오버플로우 되었을 수 있습니다.
       각 스레드는 4 kB 미만의 스택을 가지고 있으므로, 몇 개의 큰 자동 배열이나
       적당한 재귀 호출이 스택 오버플로우를 일으킬 수 있습니다. */
    ASSERT(is_thread(t));
    ASSERT(t->status == THREAD_RUNNING);

    return t;
}

/* 실행 중인 스레드의 tid를 반환합니다. */
tid_t thread_tid(void) {
    return thread_current()->tid;
}

/* 현재 스레드를 스케줄에서 제거하고 파괴합니다. 호출자에게 절대 반환되지 않습니다. */
void thread_exit(void) {
    ASSERT(!intr_context());

#ifdef USERPROG
    process_exit();
#endif
    /* 단순히 우리의 상태를 dying으로 설정하고 다른 프로세스를 스케줄합니다.
       schedule_tail() 호출 중에 우리는 파괴될 것입니다. */
    /* Just set our status to dying and schedule another process.
       We will be destroyed during the call to schedule_tail(). */
    intr_disable();
    do_schedule(THREAD_DYING);
    NOT_REACHED();
}

/* CPU를 양보합니다. 현재 스레드는 대기 상태로 만들어지지 않으며, 스케줄러의 재량에 따라 즉시 다시 스케줄될 수 있습니다. */
void thread_yield(void) {
    struct thread *curr = thread_current();
    enum intr_level old_level;

    ASSERT(!intr_context());

    old_level = intr_disable();
    if (curr != idle_thread) {
        list_insert_ordered(&ready_list, &curr->elem, cmp_priority, NULL);  // for priority...
    }

    do_schedule(THREAD_READY);
    intr_set_level(old_level);
}

void set_wake_up_time(struct thread *t, int64_t ticks) {
    t->wake_up_time = ticks;
}

void thread_sleep(int64_t ticks) {
    struct thread *curr = thread_current();
    enum intr_level old_level;

    ASSERT(!intr_context());

    old_level = intr_disable();

    if (curr != idle_thread) {
        set_wake_up_time(curr, ticks);
        list_insert_ordered(&sleep_list, &curr->elem, cmp_wake_up_time, NULL);  // for priority...
        if (ticks < min_wake_ticks) {
            min_wake_ticks = ticks;
        }
    }

    thread_block();  // thread_current의 status를 BLOCKED로, schedule()진행. 순서 굉장히 중요.
    intr_set_level(old_level);
}

void thread_wakeup(int64_t ticks) {
    if (ticks < min_wake_ticks)
        return;

    int64_t next_min_wake_ticks = INT64_MAX;

    struct list_elem *e = list_begin(&sleep_list);
    for (e; e != list_end(&sleep_list);) {
        struct thread *t = list_entry(e, struct thread, elem);
        if (t->wake_up_time <= ticks) {
            e = list_remove(e);
            thread_unblock(t);  // status를 READY로 바꾸고, ready_list에 push까지 하는함수.

        } else {
            struct list_elem *k = list_front(&sleep_list);
            struct thread *sleep_front = list_entry(k, struct thread, elem);
            min_wake_ticks = sleep_front->wake_up_time;
            break;
        }
    }
}

void thread_switching(void) {
    if (list_empty(&ready_list))
        return;
    int now_priority = thread_get_priority();
    struct list_elem *e = list_front(&ready_list);
    struct thread *ready_front = list_entry(e, struct thread, elem);
    int new_priority = ready_front->priority;

    if (new_priority > now_priority) {
        // switching 진행!
        thread_yield();
    }
}

/* 현재 스레드의 우선순위를 NEW_PRIORITY로 설정합니다. */
void thread_set_priority(int new_priority) {
    if (!thread_mlfqs) {
    thread_current()->priority = new_priority;
    thread_current()->original_priority = new_priority; //간과하면 큰일난다.
    donate_priority();
    restore_priority(); //donate_priority()와 restore_priority()순서는 중요하지 않음. 결과가 똑같음.
    thread_switching();
    }
    return;
}

/* 현재 스레드의 우선순위를 반환합니다. */
int thread_get_priority(void) {
    return thread_current()->priority;
}

/* 현재 스레드의 nice 값을 NICE로 설정합니다. */
void thread_set_nice(int Nice) {
    struct thread* cur = thread_current();
    enum intr_level old_level = intr_disable();
    cur -> nice = Nice;
    mlfqs_priority(cur);
    thread_switching();
    intr_set_level(old_level);
    return;
}

/* 현재 스레드의 nice 값을 반환합니다. */
int thread_get_nice(void) {
    struct thread* cur = thread_current();
    int get_nice;
    enum intr_level old_level = intr_disable();
    get_nice = cur->nice;
    intr_set_level(old_level);
    return get_nice;
}

//* 시스템 평균 부하의 100배를 반환합니다. */
int thread_get_load_avg(void) {
    int get_load_avg;
    enum intr_level old_level = intr_disable();
    get_load_avg = fp_to_int_round(load_avg * 100);  //반올림하여 나타내야함.
    intr_set_level(old_level);
    return get_load_avg;
}


/* 현재 스레드의 recent_cpu 값의 100배를 반환합니다. */
int thread_get_recent_cpu(void) {
    struct thread* cur = thread_current();
    int get_recent_cpu;
    enum intr_level old_level = intr_disable();
    get_recent_cpu = fp_to_int_round(mult_mixed(cur->recent_cpu ,100));
    intr_set_level(old_level);
    
    return get_recent_cpu;
}
void mlfqs_priority (struct thread *t) {
    //priority = PRI_MAX – (recent_cpu / 4) – (nice * 2)
    if (t == idle_thread)
        return;
    
        // recent_cpu는 실수,  nice는 그냥 정수라서 함수로 계산 필요 x
        //t->priority = PRI_MAX - fp_to_int_round(add_mixed(div_mixed(t->recent_cpu, 4) ,(t->nice * 2)));
        
        t->priority = PRI_MAX - fp_to_int_round(div_mixed(t->recent_cpu, 4)) - (t->nice * 2);
    }
           

void mlfqs_recent_cpu (struct thread *t) {
    //recent_cpu = (2 * load_avg) / (2 * load_avg + 1) * recent_cpu + nice
    if (t == idle_thread)
        return;
    
        //recent_cpu는 실수, load_avg는 정수.
        int decay_1 = mult_mixed(load_avg, 2);
        int decay = div_fp(decay_1, add_mixed(decay_1, 1));
        int new_recent_cpu = mult_fp(decay, t->recent_cpu);
        new_recent_cpu = add_mixed(new_recent_cpu, t->nice);
        t->recent_cpu = new_recent_cpu;
}

void mlfqs_load_avg (void) {
    //load_avg = (59/60) * load_avg + (1/60) * ready_threads
    int ready_threads;

    if (thread_current() != idle_thread) {
        ready_threads = 1 + (int)list_size(&ready_list);
    
    }
    else {  //애초에 idle_thread면 ready가 아무것도 없지 않을까? 틀린건 아니지만 불필요한 조건문?
        ready_threads = (int)list_size(&ready_list);
       
    }
    //load_avg -> 실수, ready_threads -> 정수
    int coefficient1 = div_fp(int_to_fp(59), int_to_fp(60));
    int coefficient2 = div_fp(F, int_to_fp(60));
    int term1 = mult_fp(coefficient1, load_avg);
    int term2 = mult_mixed(coefficient2, ready_threads);

    load_avg = add_fp(term1, term2);
    
}
void mlfqs_increment (void) {
    struct thread * cur = thread_current();
    if (cur != idle_thread) {
        int recent_Cpu = add_mixed(cur->recent_cpu, 1);
        cur->recent_cpu = recent_Cpu;
    }
    return;
}
void mlfqs_recalc (void) {
    struct list_elem *e = list_begin(&ready_list);
    struct thread *t;
    struct thread *cur = thread_current();
    mlfqs_recent_cpu(cur);
    if (!list_empty(&ready_list)) {
        for (e; e != list_end(&ready_list); e = list_next(&ready_list)) {
            t = list_entry(e, struct thread, elem);
            mlfqs_recent_cpu(t);
            mlfqs_priority(t);
        }
        
    }
    
    if (!list_empty(&sleep_list)) {
        e = list_begin(&sleep_list);
        for (e; e != list_end(&sleep_list); e = list_next(&sleep_list)) {
            t = list_entry(e, struct thread, elem);
            mlfqs_recent_cpu(t);
            mlfqs_priority(t);        
        } 
    }
}


/* 유휴 스레드. 다른 스레드가 실행할 준비가 되어 있지 않을 때 실행됩니다.

   유휴 스레드는 처음에 thread_start()에 의해 준비 목록에 넣어집니다.
   처음으로 한 번 스케줄되고, 그 때 유휴 스레드는 idle_thread를 초기화하고,
   thread_start()가 계속될 수 있도록 전달된 세마포어를 "up"하고 즉시 차단됩니다.
   그 후에, 유휴 스레드는 준비 목록에 나타나지 않습니다.
   준비 목록이 비어있을 경우 next_thread_to_run()에서 특별한 경우로 반환됩니다. */
static void
idle(void *idle_started_ UNUSED) {
    struct semaphore *idle_started = idle_started_;

    idle_thread = thread_current();
    sema_up(idle_started);

    for (;;) {
        /* 다른 스레드에게 실행을 양보합니다. */
        intr_disable();
        thread_block();

        /* 인터럽트를 다시 활성화하고 다음 인터럽트를 기다립니다.

           `sti` 명령어는 다음 명령어가 완료될 때까지 인터럽트를 비활성화합니다.
           따라서 이 두 명령어는 원자적으로 실행됩니다. 이러한 원자성은 중요합니다;
           그렇지 않으면 인터럽트를 다시 활성화하고 다음 인터럽트가 발생하기를 기다리는
           사이에 인터럽트가 처리될 수 있으며, 이로 인해 최대 한 클록 틱의 시간을
           낭비할 수 있습니다.

           [IA32-v2a] "HLT", [IA32-v2b] "STI", 및 [IA32-v3a] 7.11.1 "HLT 명령어"를 참조하십시오. */
        asm volatile("sti; hlt" : : : "memory");
    }
}

/* 커널 스레드의 기초로 사용되는 함수. */
static void
kernel_thread(thread_func *function, void *aux) {
    ASSERT(function != NULL);

    intr_enable(); /* 스케줄러는 인터럽트가 꺼진 상태에서 실행됩니다. */
    function(aux); /* 스레드 함수를 실행합니다. */
    thread_exit(); /* function()이 반환되면 스레드를 종료합니다. */
}

/* T를 차단된 스레드로 기본 초기화하고 NAME이라는 이름을 지정합니다. */
static void
init_thread(struct thread *t, const char *name, int priority) {
    ASSERT(t != NULL);
    ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
    ASSERT(name != NULL);

    memset(t, 0, sizeof *t);
    t->status = THREAD_BLOCKED;
    strlcpy(t->name, name, sizeof t->name);
    t->tf.rsp = (uint64_t)t + PGSIZE - sizeof(void *);
    t->priority = priority;
    t->magic = THREAD_MAGIC;
    t->original_priority = priority;
    list_init(&t->donation_list);
    t->waiting_lock = NULL;
    t->nice = NICE_DEFAULT;
    t->recent_cpu = RECENT_CPU_DEFAULT;
	
}

/* 스케줄할 다음 스레드를 선택하고 반환합니다. 실행 대기 큐에서 스레드를 반환해야 합니다.
   실행 대기 큐가 비어있으면 idle_thread를 반환합니다. */
static struct thread *
next_thread_to_run(void) {
    if (list_empty(&ready_list))
        return idle_thread;
    else
        return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

struct list_elem *get_list_head() {
    return &ready_list.head;
}

/* iretq를 사용하여 스레드를 시작합니다. */
void do_iret(struct intr_frame *tf) {
    __asm __volatile(
        "movq %0, %%rsp\n"
        "movq 0(%%rsp),%%r15\n"
        "movq 8(%%rsp),%%r14\n"
        "movq 16(%%rsp),%%r13\n"
        "movq 24(%%rsp),%%r12\n"
        "movq 32(%%rsp),%%r11\n"
        "movq 40(%%rsp),%%r10\n"
        "movq 48(%%rsp),%%r9\n"
        "movq 56(%%rsp),%%r8\n"
        "movq 64(%%rsp),%%rsi\n"
        "movq 72(%%rsp),%%rdi\n"
        "movq 80(%%rsp),%%rbp\n"
        "movq 88(%%rsp),%%rdx\n"
        "movq 96(%%rsp),%%rcx\n"
        "movq 104(%%rsp),%%rbx\n"
        "movq 112(%%rsp),%%rax\n"
        "addq $120,%%rsp\n"
        "movw 8(%%rsp),%%ds\n"
        "movw (%%rsp),%%es\n"
        "addq $32, %%rsp\n"
        "iretq"
        : : "g"((uint64_t)tf) : "memory");
}

/* 스레드를 전환하여 새 스레드의 페이지 테이블을 활성화하고,
   이전 스레드가 죽어가고 있다면 파괴합니다.

   이 함수를 호출할 때, 우리는 방금 스레드 PREV에서 전환했고,
   새 스레드가 이미 실행 중이며, 인터럽트는 여전히 비활성화된 상태입니다.

   스레드 전환이 완료될 때까지 printf()를 호출하는 것은 안전하지 않습니다.
   실제로 이는 printf()를 함수의 끝에 추가해야 함을 의미합니다. */
static void
thread_launch(struct thread *th) {
    uint64_t tf_cur = (uint64_t)&running_thread()->tf;
    uint64_t tf = (uint64_t)&th->tf;
    ASSERT(intr_get_level() == INTR_OFF);

    /* 주 스위칭 로직.
     * 우리는 먼저 전체 실행 컨텍스트를 intr_frame에 복원하고
     * do_iret을 호출하여 다음 스레드로 전환합니다.
     * 주의할 점은, 전환 작업이 완료될 때까지 여기서 스택을 사용해서는 안 됩니다. */
    __asm __volatile(
        /* 사용될 레지스터들을 저장합니다. */
        "push %%rax\n"
        "push %%rbx\n"
        "push %%rcx\n"
        /* 입력을 한 번 가져옵니다 */
        "movq %0, %%rax\n"
        "movq %1, %%rcx\n"
        "movq %%r15, 0(%%rax)\n"
        "movq %%r14, 8(%%rax)\n"
        "movq %%r13, 16(%%rax)\n"
        "movq %%r12, 24(%%rax)\n"
        "movq %%r11, 32(%%rax)\n"
        "movq %%r10, 40(%%rax)\n"
        "movq %%r9, 48(%%rax)\n"
        "movq %%r8, 56(%%rax)\n"
        "movq %%rsi, 64(%%rax)\n"
        "movq %%rdi, 72(%%rax)\n"
        "movq %%rbp, 80(%%rax)\n"
        "movq %%rdx, 88(%%rax)\n"
        "pop %%rbx\n"  // 저장된  rcx
        "movq %%rbx, 96(%%rax)\n"
        "pop %%rbx\n"  // 저장된  rbx
        "movq %%rbx, 104(%%rax)\n"
        "pop %%rbx\n"  // 저장된  rax
        "movq %%rbx, 112(%%rax)\n"
        "addq $120, %%rax\n"
        "movw %%es, (%%rax)\n"
        "movw %%ds, 8(%%rax)\n"
        "addq $32, %%rax\n"
        "call __next\n"  // 현재 rip를 읽습니다.
        "__next:\n"
        "pop %%rbx\n"
        "addq $(out_iret -  __next), %%rbx\n"
        "movq %%rbx, 0(%%rax)\n"  // rip
        "movw %%cs, 8(%%rax)\n"   // cs
        "pushfq\n"
        "popq %%rbx\n"
        "mov %%rbx, 16(%%rax)\n"  // eflags
        "mov %%rsp, 24(%%rax)\n"  // rsp
        "movw %%ss, 32(%%rax)\n"
        "mov %%rcx, %%rdi\n"
        "call do_iret\n"
        "out_iret:\n"
        : : "g"(tf_cur), "g"(tf) : "memory");
}

/* 새 프로세스를 스케줄합니다. 진입 시, 인터럽트는 꺼져 있어야 합니다.
 * 이 함수는 현재 스레드의 상태를 status로 변경한 다음 다른 스레드를 찾아 전환합니다.
 * schedule()에서 printf()를 호출하는 것은 안전하지 않습니다. */
static void
do_schedule(int status) {
    ASSERT(intr_get_level() == INTR_OFF);
    ASSERT(thread_current()->status == THREAD_RUNNING);
    while (!list_empty(&destruction_req)) {
        struct thread *victim =
            list_entry(list_pop_front(&destruction_req), struct thread, elem);
        palloc_free_page(victim);
    }
    thread_current()->status = status;
    schedule();
}

static void
schedule(void) {
    struct thread *curr = running_thread();
    struct thread *next = next_thread_to_run();

    ASSERT(intr_get_level() == INTR_OFF);
    ASSERT(curr->status != THREAD_RUNNING);
    ASSERT(is_thread(next));
    /* 우리를 실행 중으로 표시합니다. */
    next->status = THREAD_RUNNING;

    /* 새 타임 슬라이스를 시작합니다. */
    thread_ticks = 0;

#ifdef USERPROG
    /* 새 주소 공간을 활성화합니다. */
    process_activate(next);
#endif

    if (curr != next) {
        /* 우리가 전환하는 스레드가 죽어가고 있다면, 그 스레드의 구조체를 파괴합니다.
           이 작업은 thread_exit()가 자신을 종료하는 것을 방지하기 위해 늦게 일어나야 합니다.
           현재 스택에서 페이지가 사용되고 있으므로 여기서 페이지 해제 요청을 큐에 넣습니다.
           실제 파괴 로직은 schedule()의 시작 부분에서 호출됩니다. */
        if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
            ASSERT(curr != next);
            list_push_back(&destruction_req, &curr->elem);
        }

        /* 스레드를 전환하기 전에, 현재 실행 중인 정보를 먼저 저장합니다. */
        thread_launch(next);
    }
}

/* 새 스레드에 사용할 tid를 반환합니다. */
static tid_t
allocate_tid(void) {
    static tid_t next_tid = 1;
    tid_t tid;

    lock_acquire(&tid_lock);
    tid = next_tid++;
    lock_release(&tid_lock);

    return tid;
}

/* ready_list를 thread 구조체*/
void print_ready_list(void) {
    printf("Ready list is ");
    if (!list_empty(&ready_list)) {
        struct list_elem *e;

        for (e = list_begin(&ready_list); e != list_end(&ready_list); e = list_next(e)) {
            if (e == list_tail(&ready_list)) {
                break;  // 리스트의 끝에 도달하면 반복문 종료
            }
            struct thread *t = list_entry(e, struct thread, elem);
            printf("Thread name: %s,  status: %d   ", t->name, t->status);
        }
    }
    printf("\n");
}

/* sleep_list를 thread 구조체*/
void print_sleep_list(void) {
    printf("Sleep list is ");
    if (!list_empty(&sleep_list)) {
        struct list_elem *e;

        for (e = list_begin(&sleep_list); e != list_end(&sleep_list); e = list_next(e)) {
            if (e == list_tail(&sleep_list)) {
                break;  // 리스트의 끝에 도달하면 반복문 종료
            }
            struct thread *t = list_entry(e, struct thread, elem);
            printf("Thread name: %s,  status: %d   ", t->name, t->status);
        }
    }
    printf("\n");
}