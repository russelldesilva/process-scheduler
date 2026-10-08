#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <unistd.h>
#include <regex.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

typedef struct Process {
    pid_t PID;
    int code;
    char *file_name;
    char *n;
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
// all_queue contains all processes across all states, even those that are not in the other queues
P_Node_Queue *all_queue, *ready_queue, *running_queue, *blocked_queue;

// regex comparators for priority and event args in run()
regex_t priority_re, event_re;

// regex comparator for PID
regex_t pid_re;

// path to dummy program
char *PROGRAM_PATH;

// indicates whether a child is done.
// 1 = done, 0 = not done
volatile sig_atomic_t child_done = 0;

// handler for signal when child is done
void on_sigchld(int sig) {
    child_done = 1;                     
}

// extracts all arguements into an array of strings
// - args: the output array, num_args: the MAX number of arguements for a method
// - returns number of args actually parsed
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

// add a process to the end of a queue
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

// // assings a process to either running or ready queue
// // - will send to running queue only if there are free slots (max 3)
// // - otherwise send to ready queue
// void assign_to_queue(Process *process) {
//     if (running_queue->size < 3) {
//         process->code = 0;
//         append_to_queue(running_queue, process);
//     } else {
//         process->code = 1;
//         append_to_queue(ready_queue, process);
//     }
// }

// finds a process by its pid, returns that process or NULL if not found
Process *find_by_pid(P_Node_Queue *queue, pid_t pid) {
    Process *curr = queue->head;

    while (curr != NULL) {
        if (curr->PID == pid) {
            return curr;
        }
        curr = curr->global_next;
    }

    return NULL;
}

// Removes the given process from queue. Returns it, or NULL if it is not in the queue.
Process *remove_from_queue(P_Node_Queue *queue, Process *target) {
    Process **curr = &queue->head;

    while (*curr != NULL) {
        if (*curr == target) {
            Process *removed = *curr;
            *curr = removed->next;
            removed->next = NULL;
            queue->size--;
            return removed;
        }
        curr = &(*curr)->next;
    }
    return NULL;
}

// sends a signal to OS process
// pid: PID of process, signal: SIGSTOP, SIGKILL or SIGCONT
int send_signal_to_process(pid_t pid, int signal) {
    Process *process = find_by_pid(all_queue, pid);

    if (process == NULL) {
        printf("Invalid PID\n");
        return -1;
    }

    if (signal == SIGSTOP && process->code != 0) {
        printf("Unable to stop a process that is not in the running state (code: 0)\n");
        return -1;
    } else if (signal == SIGKILL && process->code == 3) {
        printf("Unable to kill a process that has already terminated (code: 3)\n");
        return -1;
    } else if (signal == SIGCONT && process->code != 2) {
        printf("Unable to resume a process that is not in the stopped state (code: 2)\n");
        return -1;
    }

    int status = kill(pid, signal);
    if (signal == SIGKILL) {
        waitpid(pid, NULL, 0); // wait for child process to die first before garbage collection
    }

    if (status == -1) {
        perror("Error sending signal to process");
        return -1;
    }

    return 0;
}

// void print_queue(P_Node_Queue *queue) {
//     Process *curr = queue->head;
//     printf("%-8s %-6s %s\n", "PID", "STATE", "PRIORITY");
//     while (curr != NULL) {
//         printf("%-8d %-6d %s\n", curr->PID, curr->code, curr->priority);
//         curr = curr->next;
//     }
// }

// prints all processes in the all_queue
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

// parses priority string into an integer, -1 if it fails.
// P1 -> 1, P2 -> 2, ... Pn -> n
int parse_priority(char *priority) {
    char *end;
    long value = strtol(priority + 1, &end, 10);

    if (end == priority + 1 || *end != '\0') {
        return -1;
    }

    return (int)value;
}

// picks the next process to run from the ready queue, returns selected process of NULL if queue is empty
Process *pick_next_process() {
    if (ready_queue->head == NULL) {
        return NULL;
    }

    int highest_priority = parse_priority(ready_queue->head->priority);
    Process *curr = ready_queue->head;

    // find highest priority (smallest digit) in the queue
    while (curr != NULL) {
        int curr_priority = parse_priority(curr->priority);
        if (curr_priority != -1 && curr_priority < highest_priority) {
            highest_priority = curr_priority;
        }
        curr = curr->next;
    }

    curr = ready_queue->head;
    // greedy: find first occurrence of highest priority as that will be the process that arrived first
    while (curr != NULL) {
        int curr_priority = parse_priority(curr->priority);
        if (curr_priority != -1 && curr_priority == highest_priority) {
            return curr;
        }
        curr = curr->next;
    }

    return NULL;
}

// start executing a new process
void start_process(Process *process) {
    pid_t pid = fork(); 
    if (pid == 0) {
        char *prog_args[] = {PROGRAM_PATH, process->file_name, process->n, NULL};
        execv(PROGRAM_PATH, prog_args);
        perror("Unable to execute program");    // only reached if exec failed
        _exit(1);
    } else {
        process->PID = pid; // assign acutal PID to process object
    }
}

// start/resume a chosen process from the ready queue.
void dispatch() {
    while (running_queue->size < 3) {
        Process *chosen_one = pick_next_process();
        if (chosen_one == NULL) {
            return;
        }

        remove_from_queue(ready_queue, chosen_one);

        if (chosen_one->PID == 0) { // PID = 0 implies a new process
            start_process(chosen_one);
        } else { // else it is a process returning from stop
            chosen_one->code = 2;
            send_signal_to_process(chosen_one->PID, SIGCONT);
        }

        append_to_queue(running_queue, chosen_one);
        chosen_one->code = 0;
    }
}

void handle_dead_children() {
    pid_t dead_child;

    // waitpid: 
    // -1 means wait for any child
    // NULL is just a placeholder to store the status of the child's terminated execution
    // WNOHANG means keep running if no children have finished
    // return PID of child that died
    while ((dead_child = waitpid(-1, NULL, WNOHANG)) > 0) {
        Process *p = find_by_pid(all_queue, dead_child);
        if (p == NULL) {
            continue;
        }
        p->code = 3;
        remove_from_queue(running_queue, p);
    }
}

int main(void) {
    // program path
    PROGRAM_PATH = "./out/prog";

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
    regcomp(&pid_re, "^[1-9][0-9]{0,6}$", REG_EXTENDED | REG_NOSUB);

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

    // register the handler to listen for signals from children
    struct sigaction sa = {0};
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    
    // buffer for command input
    char line[256];
    
    // gate for the prompt: only print it when the previous one has been consumed by a
    // completed line. If fgets is interrupted by SIGCHLD, the prompt (and whatever the
    // user has typed so far) is still on screen, so we must not print it again.
    bool show_prompt = true;

    while (true) {
        handle_dead_children();
        dispatch();

        if (show_prompt) {
            printf("cs205$ ");
            fflush(stdout);
            show_prompt = false;
        }

        if (fgets(line, sizeof line, stdin) == NULL) {
            if (errno == EINTR) {
                clearerr(stdin);
                continue;   // interrupted: keep waiting without reprinting the prompt
            }
            break;
        }

        show_prompt = true; // a full line was read, so the next iteration needs a fresh prompt

        // replace \n with \0 so strtok knows the end of the line
        line[strcspn(line, "\n")] = '\0';

        // extract command (run, stop, kill, etc...)
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
                Process *new_process = malloc(sizeof(Process));
                new_process->PID = 0;
                new_process->code = 1;
                new_process->file_name = strdup(args[1]);
                new_process->n = strdup(args[2]);
                new_process->priority = strdup(args[3]);

                // handle optional event arg
                if (num_args == 5) {
                    new_process->event = strdup(args[4]);
                } else {
                    new_process->event = NULL;
                }

                append_to_global_queue(all_queue, new_process);

                // new processes go to ready queue first and dispatched soon after
                append_to_queue(ready_queue, new_process); 
            }
        } else if (strcmp(command, STOP_CMD) == 0) {
            char *args[STOP_ARGS + 1];
            int num_args = extract_args(args, STOP_ARGS);

            if (num_args < STOP_ARGS) {
                printf("Usage: stop <PID>\n");
            } else if (regexec(&pid_re, args[0], 0, NULL, 0) != 0) {
                printf("Invalid PID\n");
            } else {
                pid_t pid = strtol(args[0], NULL, 10);
                if (send_signal_to_process(pid, SIGSTOP) == 0) {
                    printf("stopping %d\n", pid);
                    Process *stopped = find_by_pid(all_queue, pid);
                    remove_from_queue(running_queue, stopped);
                    stopped->code = 2;
                }
            }
        } else if (strcmp(command, KILL_CMD) == 0) {
            char *args[KILL_ARGS + 1];
            int num_args = extract_args(args, KILL_ARGS);

            if (num_args < KILL_ARGS) {
                printf("Usage: kill <PID>\n");
            } else if (regexec(&pid_re, args[0], 0, NULL, 0) != 0) {
                printf("Invalid PID\n");
            } else {
                pid_t pid = strtol(args[0], NULL, 10);
                if (send_signal_to_process(pid, SIGKILL) == 0) {
                    printf("terminating %d\n", pid);
                    Process *terminated = find_by_pid(all_queue, pid);
                    terminated->code = 3;

                    remove_from_queue(running_queue, terminated);
                    remove_from_queue(blocked_queue, terminated);
                    remove_from_queue(ready_queue, terminated);
                }
            }
        } else if (strcmp(command, RESUME_CMD) == 0) {
            char *args[RESUME_ARGS + 1];
            int num_args = extract_args(args, RESUME_ARGS);

            if (num_args < RESUME_ARGS) {
                printf("Usage: resume <PID>\n");
            } else if (regexec(&pid_re, args[0], 0, NULL, 0) != 0) {
                printf("Invalid PID\n");
            } else {
                pid_t pid = strtol(args[0], NULL, 10);
                Process *resumed = find_by_pid(all_queue, pid);
                if (resumed) {
                    if (resumed->code == 2) {
                        printf("resuming %d\n", pid);
                        append_to_queue(ready_queue, resumed);
                        resumed->code = 1;
                    } else {
                        printf("Unable to resume a process that is not in the stopped state (code: 2)\n");
                    }
                } else {
                    printf("Invalid PID\n");
                }
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