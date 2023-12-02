#include "stdint.h"
<<<<<<< HEAD

#define F (1 << 14) //fixed point 1
#define INT_MAX ((1 << 31) - 1)
#define INT_MIN (-(1 << 31))
// x and y denote fixed_point numbers in 17.14 format
// n is an integer
int int_to_fp(int n); /* integer를 fixed point로 전환 */
int fp_to_int_round(int x); /* FP를 int로 전환(반올림) */
int fp_to_int(int x); /* FP를 int로 전환(버림) */
int add_fp(int x, int y); /* FP의 덧셈 */
int add_mixed(int x, int n); /* FP와 int의 덧셈 */
int sub_fp(int x, int y); /* FP의 뺄셈(x-y) */
int sub_mixed(int x, int n); /* FP와 int의 뺄셈(x-n) */
int mult_fp(int x, int y); /* FP의 곱셈 */
int mult_mixed(int x, int y); /* FP와 int의 곱셈 */
int div_fp(int x, int y); /* FP의 나눗셈(x/y) */
int div_mixed(int x, int n); /* FP와 int 나눗셈(x/n) */


int fp_to_int(int x) {
    return x / F;
=======
#define F (1 << 14) // fixed point 로 쓴 1
#define INT_MAX ((1 <<31) - 1)
#define INT_MIN (-(1 << 31))

int int_to_fp(int n);
int fp_to_int_round(int x); // 반올림
int fp_to_int(int x); // 버림
int add_fp(int x, int y);
int add_mixed(int x, int n);
int sub_fp(int x, int y);
int sub_mixed(int x, int n);
int mult_fp(int x, int y);
int mult_mixed(int x, int n);
int div_fp(int x, int y);
int div_mixed(int x, int n);

int int_to_fp(int n) {
    return n * F;
>>>>>>> 9e75962891ced912555253a80a14667e4a903287
}

int fp_to_int_round(int x) {
    if (x >= 0) {
<<<<<<< HEAD
        return (x + F / 2) / F;
    } else {
        return (x - F / 2) / F;
    }
=======
        return (x + F/2) / F;
    }
    else {
        return (x - F/2) / F;
    }
}

int fp_to_int(int x) {
    return x/F;
>>>>>>> 9e75962891ced912555253a80a14667e4a903287
}

int add_fp(int x, int y) {
    return x + y;
}

int add_mixed(int x, int n) {
<<<<<<< HEAD
    return x + int_to_fp(n);
=======
    return x + n * F;
>>>>>>> 9e75962891ced912555253a80a14667e4a903287
}

int sub_fp(int x, int y) {
    return x - y;
}

int sub_mixed(int x, int n) {
<<<<<<< HEAD
    return x - int_to_fp(n);
}

int mult_fp(int x, int y) {
    return ((int64_t)x) * y / F;
}

int mult_mixed(int x, int n) {
    return x * n;
}

int div_fp(int x, int y) {
    return ((int64_t)x) * F / y;
}

int div_mixed(int x, int n) {
    return x / n;
}

int int_to_fp(int n) {
    return n * F;
=======
    return x - n * F;
}
int mult_fp(int x, int y) {
    return ((int64_t) x ) * y / F;
}
int mult_mixed(int x, int n) {
    return x * n;
}
int div_fp(int x, int y) {
    return ((int64_t) x ) * F / y;
}
int div_mixed(int x, int n) {
    return x / n;
>>>>>>> 9e75962891ced912555253a80a14667e4a903287
}