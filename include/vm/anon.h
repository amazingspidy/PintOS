#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
struct page;
enum vm_type;

/*지금 이 구조체는 텅 비어 있지만, 당신이 구현하면서 필요한 정보나 익명 페이지의
 * 상태를 저장하기 위해 멤버를 추가할 수 있습니다.*/
struct anon_page {};

void vm_anon_init(void);
bool anon_initializer(struct page *page, enum vm_type type, void *kva);

#endif