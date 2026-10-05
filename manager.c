#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct Process {
    int PID;
    int code;
    char *priority;
    char *event;
    struct Process *next;
    struct Process *global_next;
} Process;

typedef struct P_Node_Queue {
    int size;
    struct Process *head;
} P_Node_Queue;

// Global vars (process queues)
P_Node_Queue *all_queue, *ready_queue, *running_queue, *blocked_queue;

int extract_args(char* args[], int num_args) {
    int idx = 0;
    char *arg;

    while (idx < num_args && (arg = strtok(NULL, " ")) != NULL) {
        // printf("%s\n", arg);
        args[idx] = arg;
        idx++;
    }

    args[idx] = NULL;
    return idx;
}

void append_to_queue(P_Node_Queue *queue, Process *process) {
    process->next = NULL;

    if (queue->head == NULL) {
        queue->head = process;
    } else {
        Process *curr = queue->head;
        while (curr->next != NULL) {
            curr = curr->next;
        }
        curr->next = process;
    }

    queue->size++;
}

// Same as append_to_queue, but links through global_next so a process can be
// in all_queue and in one of the state queues at the same time.
void append_to_global_queue(P_Node_Queue *queue, Process *process) {
    process->global_next = NULL;

    if (queue->head == NULL) {
        queue->head = process;
    } else {
        Process *curr = queue->head;
        while (curr->global_next != NULL) {
            curr = curr->global_next;
        }
        curr->global_next = process;
    }

    queue->size++;
}

void assign_to_queue(Process *process, int PID) {
    if (running_queue->size < 3) {
        process->code = 0;
        append_to_queue(running_queue, process);
    } else {
        process->code = 1;
        append_to_queue(ready_queue, process);
    }
}

// void print_queue(P_Node_Queue *queue) {
//     Process *curr = queue->head;
//     printf("%-8s %-6s %s\n", "PID", "STATE", "PRIORITY");
//     while (curr != NULL) {
//         printf("%-8d %-6d %s\n", curr->PID, curr->code, curr->priority);
//         curr = curr->next;
//     }
// }

void print_global_queue(P_Node_Queue *queue) {
    Process *curr = queue->head;
    printf("%-8s %-6s %s\n", "PID", "STATE", "PRIORITY");
    while (curr != NULL) {
        printf("%-8d %-6d %s\n", curr->PID, curr->code, curr->priority);
        curr = curr->global_next;
    }
}

int main(void) {
    const char RUN_CMD[4] = "run";
    const char STOP_CMD[5] = "stop";
    const char KILL_CMD[5] = "kill";
    const char RESUME_CMD[7] = "resume";
    const char LIST_CMD[5] = "list";
    const char EXIT_CMD[5] = "exit";
    const char EVENT_CMD[6] = "event";
    
    const int RUN_ARGS = 5;

    all_queue = malloc(sizeof(P_Node_Queue));
    all_queue->size = 0;
    all_queue->head = NULL;

    ready_queue = malloc(sizeof(P_Node_Queue));
    ready_queue->size = 0;
    ready_queue->head = NULL;

    blocked_queue = malloc(sizeof(P_Node_Queue));
    blocked_queue->size = 0;
    blocked_queue->head = NULL;

    running_queue = malloc(sizeof(P_Node_Queue));
    running_queue->size = 0;
    running_queue->head = NULL;
    
    char line[256];
    int PID = 0;
    
    while (true) {
        printf("cs205$ ");
        fflush(stdout);

        if (fgets(line, sizeof line, stdin) == NULL) {
            break;
        }

        // replace \n with \0
        line[strcspn(line, "\n")] = '\0';

        char *command = strtok(line, " ");
        if (command == NULL) {
            continue;
        }

        if (strcmp(command, RUN_CMD) == 0) {
            char *args[RUN_ARGS + 1];
            int num_args = extract_args(args, RUN_ARGS);
            // args[0] = "prog"
            // args[1] = <file_name>
            // args[2] = <n>
            // args[3] = <priority>
            // args[4] = <event>
            // args[5] = NULL

            if (num_args < 3) {
                printf("Usage: run prog <file_name> <n> [priority] [event]\n");
            } else if (strcmp(args[0], "prog") != 0) {
                printf("Invalid program. Use prog\n");
            } else {
                args[0] = "./out/prog";

                char *prog_args[] = {args[0], args[1], args[2], NULL};

                Process *new_process = malloc(sizeof(Process));
                new_process->PID = ++PID;
                new_process->priority = strdup(args[3]);
                new_process->event = strdup(args[4]);
                append_to_global_queue(all_queue, new_process);
                assign_to_queue(new_process, PID);

                if (new_process->code == 0) {
                    pid_t pid = fork();
                    if (pid == 0) {
                        execv(args[0], prog_args);
                        perror("execv");    // only reached if exec failed
                        _exit(1);
                    }
                }
            }
        } else if (strcmp(command, STOP_CMD) == 0) {
        
        } else if (strcmp(command, KILL_CMD) == 0) {
        
        } else if (strcmp(command, RESUME_CMD) == 0) {
        
        } else if (strcmp(command, LIST_CMD) == 0) {
            print_global_queue(all_queue);
        } else if (strcmp(command, EXIT_CMD) == 0) {

        } else if (strcmp(command, EVENT_CMD) == 0) {
        
        } else {
            printf("Invalid command\n");
        }
    }
}