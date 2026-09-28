// target.c
#include <stdio.h>

void calculate_values(int factor) {
    int secret_number = 42; 
    volatile int stack_anchor = 0; 
    
    int result = factor * secret_number; 
    printf("[Target Output] Result calculated: %d\n", result);

    int secret_value = 10 + result; // <-- THIS LINE MODIFIES secret_value
}

int main() {
    printf("[Target] Calling calculate_values...\n");
    calculate_values(5);
    printf("[Target] Done.\n");
    return 0;
}