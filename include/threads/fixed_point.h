#include "stdint.h"

#define INT_MAX ((1 << 31) - 1)
#define INT_MIN (-(1 << 31))
#define FP_F (1 << 14)

int int_to_fp(int);       /* Convert n to fixed point */
int fp_to_int(int);       /* Convert x to integer (rounding to nearest) */
int fp_to_int_round(int); /* Convert x to integer (rounding toward zero) */
int add_fp(int, int);     /* Add x and y */
int add_mixed(int, int);  /* Add x and n */
int sub_fp(int, int);     /* Subtract y from x */
int sub_mixed(int, int);  /* Subtract n from x */
int mult_fp(int, int);    /* Multiply x by y */
int mult_mixed(int, int); /* Multiply x by n */
int div_fp(int, int);     /* Divide x by y */
int div_mixed(int, int);  /* Divide x by n */

int fp_to_int(int x) {
    return x / FP_F;
}

int fp_to_int_round(int x) {
    if (x >= 0) {
        return (x + FP_F / 2) / FP_F;
    } else {
        return (x - FP_F / 2) / FP_F;
    }
}

int add_fp(int x, int y) {
    return x + y;
}

int add_mixed(int x, int n) {
    return x + int_to_fp(n);
}

int sub_fp(int x, int y) {
    return x - y;
}

int sub_mixed(int x, int n) {
    return x - int_to_fp(n);
}

int mult_fp(int x, int y) {
    return ((int64_t)x) * y / FP_F;
}

int mult_mixed(int x, int n) {
    return x * n;
}

int div_fp(int x, int y) {
    return ((int64_t)x) * FP_F / y;
}

int div_mixed(int x, int n) {
    return x / n;
}

int int_to_fp(int n) {
    return n * FP_F;
}
