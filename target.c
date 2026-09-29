// target.c
#include <stdio.h>

struct Player {
    int id;       // 4 bytes
    int health;   // 4 bytes
    char* name;
};

void calculate_values(int factor) {
    int secret_number = 42; 
    volatile int stack_anchor = 0; 
    
    // Test Variables for Expression Evaluation
    char* my_string = "Hello Debugger!";
    int my_array[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    int* my_ptr = &secret_number;

    int result = factor * secret_number; 
    printf("[Target Output] Result calculated: %d\n", result);

    int secret_value = 10 + result; 

    struct Player player1 = { 5, 100, "Super"};

    char* my_str = "abc";

    char *my_str_array = (char *)calloc(5, sizeof(char));
    my_str_array[0] = 'd';
    my_str_array[1] = 'e';
    my_str_array[2] = 'f';
    my_str_array[3] = 'g';
    free(my_str_array);

    secret_value += 1;
}

int main() {
    printf("[Target] Calling calculate_values...\n");
    calculate_values(5);
    printf("[Target] Done.\n");
    return 0;
}
