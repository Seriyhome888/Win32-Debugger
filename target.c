// target.c
#include <stdio.h>

void calculate_values(int factor) {
    int secret_number = 42; 
    volatile int stack_anchor = 0; 
    
    // Test Variables for Expression Evaluation
    char* my_string = "Hello Debugger!";
    int my_array[5] = {10, 20, 30, 40, 50};
    int* my_ptr = &secret_number;

    int result = factor * secret_number; 
    printf("[Target Output] Result calculated: %d\n", result);

    int secret_value = 10 + result; 

    secret_value += 1;
}

int main() {
    printf("[Target] Calling calculate_values...\n");
    calculate_values(5);
    printf("[Target] Done.\n");
    return 0;
}
