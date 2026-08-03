#define N 64

int A[N], B[N], C[N], D[N], E[N], F[N];
volatile int sink;

int benchmark(int cond) {
    int result = 0;

    int a0 = A[0]; int a1 = A[1];
    int a2 = A[2]; int a3 = A[3];
    int a4 = A[4]; int a5 = A[5];
    int base = a0 + a1 + a2 + a3 + a4 + a5;

    if (cond) {
        result += C[0] * D[0] + C[1] * D[1];
    } else {
        int e0 = E[0]; int e1 = E[1]; int e2 = E[2];
        int e3 = E[3]; int e4 = E[4]; int e5 = E[5];

        int f0 = F[0]; int f1 = F[1];
        int f2 = F[2]; int f3 = F[3];

        int r0 = base + e0;
        int r1 = r0  * e1;
        int r2 = r1  + e2;
        int r3 = r2  * e3;
        int r4 = r3  + e4;
        int r5 = r4  * e5;
        int r6 = r5  + f0;
        int r7 = r6  * f1;
        int r8 = r7  + f2;
        int r9 = r8  * f3;

        int s0 = a0*a1 + a2*a3 + a4*a5;
        result += r9 + s0;
    }

    int m0 = result + B[0] + B[1];
    int m1 = m0 * B[2];
    sink = m1;
    return m1;
}

int main() {
    for (int i = 0; i < N; i++) {
        A[i] = i + 1;
        B[i] = i * 2 + 1;
        C[i] = i * 3 + 2;
        D[i] = i * 4 + 3;
        E[i] = i * 5 + 4;
        F[i] = i * 6 + 5;
    }

    int result = 0;
    for (int i = 0; i < 1000000; i++)
        result += benchmark(i % 10 == 0);
    sink = result;
    return sink;
}
