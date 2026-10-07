#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <unistd.h>
#include <regex.h>

typedef struct Process {
    pid_t PID;
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

// === Global vars ==== 
// process queues
P_Node_Queue *all_queue, *ready_queue, *running_queue, *blocked_queue;

// regex comparators for priority and event args in run()
regex_t priority_re, event_re;

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

void assign_to_queue(Process *process) {
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
        if (curr->PID != 0) {
            printf("%-8d %-6d %s\n", curr->PID, curr->code, curr->priority);
        } else {
            printf("%-8c %-6d %s\n", '-', curr->code, curr->priority); // print '-' when PID is 0 (i.e. process not started yet)
        }
        curr = curr->global_next;
    }
}

int main(void) {
    // names of commands
    const char RUN_CMD[4] = "run";
    const char STOP_CMD[5] = "stop";
    const char KILL_CMD[5] = "kill";
    const char RESUME_CMD[7] = "resume";
    const char LIST_CMD[5] = "list";
    const char EXIT_CMD[5] = "exit";
    const char EVENT_CMD[6] = "event";
    
    // number of args for each command
    const int RUN_ARGS = 5;
    const int RUN_ARGS_MANDATORY = 4;
    const int STOP_ARGS = 1;
    const int RESUME_ARGS = 1;
    const int KILL_ARGS = 1;
    const int EVENT_ARGS = 1;

    // set regex comparators
    regcomp(&priority_re, "^P[1-9][0-9]*$", REG_EXTENDED | REG_NOSUB);
    regcomp(&event_re, "^E[1-3]@[1-9][0-9]*$", REG_EXTENDED | REG_NOSUB);

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
    
    while (true) {
        printf("cs205$ ");
        fflush(stdout);

        if (fgets(line, sizeof line, stdin) == NULL) {
            break;
        }

        // replace \n with \0 so strtok knows the end of the line
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

            if (num_args < RUN_ARGS_MANDATORY) {
                printf("Usage: run prog <file_name> <n> <priority> [event (optional)]\n");
            } else if (strcmp(args[0], "prog") != 0) {
                printf("Invalid program. Use prog\n");
            } else if (regexec(&priority_re, args[3], 0, NULL, 0) != 0) {
                printf("Invalid priority. Priority must be in the form P1, P2, P3, ... Pn\n");
            } else if (num_args == 5 && regexec(&event_re, args[4], 0, NULL, 0) != 0) {
                printf("Invalid event. Event must be in the form E1/E2/E3@<time_till_event_start>\n");
            }
            else {
                args[0] = "./out/prog";

                char *prog_args[] = {args[0], args[1], args[2], NULL};

                Process *new_process = malloc(sizeof(Process));
                new_process->PID = 0;
                new_process->priority = strdup(args[3]);

                // handle optional event arg
                if (num_args == 5) {
                    new_process->event = strdup(args[4]);
                } else {
                    new_process->event = NULL;
                }

                append_to_global_queue(all_queue, new_process);
                assign_to_queue(new_process);
                
                if (new_process->code == 0) {
                    pid_t pid = fork();
                    if (pid == 0) {
                        execv(args[0], prog_args);
                        perror("execv");    // only reached if exec failed
                        _exit(1);
                    } else {
                        new_process->PID = pid; // assign acutal PID to process object
                    }
                }
            }
        } else if (strcmp(command, STOP_CMD) == 0) {
            char *args[STOP_ARGS + 1];
            int num_args = extract_args(args, STOP_ARGS);

            if (num_args < STOP_ARGS) {
                printf("Usage: stop <PID>\n");
            } else {
                
            }
        } else if (strcmp(command, KILL_CMD) == 0) {
            char *args[KILL_ARGS + 1];
            int num_args = extract_args(args, KILL_ARGS);

            if (num_args < KILL_ARGS) {
                printf("Usage: kill <PID>\n");
            }
        } else if (strcmp(command, RESUME_CMD) == 0) {
            char *args[RESUME_ARGS + 1];
            int num_args = extract_args(args, RESUME_ARGS);

            if (num_args < RESUME_ARGS) {
                printf("Usage: resume <PID>\n");
            }
        } else if (strcmp(command, LIST_CMD) == 0) {
            print_global_queue(all_queue);
        } else if (strcmp(command, EXIT_CMD) == 0) {
            break;
        } else if (strcmp(command, EVENT_CMD) == 0) {
            char *args[EVENT_ARGS + 1];
            int num_args = extract_args(args, EVENT_ARGS);        

            if (num_args < EVENT_ARGS) {
                printf("Usage: event <E1/E2/E3>\n");
            }
        } else {
            printf("Invalid command\n");
        }
    }
}