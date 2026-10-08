/*
 * manager.c - CS205 Assignment 2: Process Management
 * Author: Russell de Silva
 *
 * An interactive process manager (prompt: cs205$) that runs programs as background
 * child processes. At most 3 run at once; the rest wait in a ready queue and are
 * dispatched by user-defined priority (P1 = highest), ties broken FCFS by arrival.
 * Scheduling is non-preemptive: a running process is only replaced when it finishes,
 * is stopped, is killed, or blocks on an event.
 *
 * Compile:  gcc -o manager manager.c
 * Run:      ./manager
 *
 * Commands:
 *   run <program> [args...] <Pn> [En@cycle]  admit a process, optionally blocking on
 *                                            event En after `cycle` seconds of running
 *   stop <PID>    suspend a running process (SIGSTOP), freeing its slot
 *   resume <PID>  move a stopped process back to the ready queue
 *   kill <PID>    terminate a process (SIGKILL)
 *   event <En>    unblock every process waiting on En (E1/E2/E3), FCFS
 *   list          show PID, state code and priority of every process
 *   exit          kill all remaining children and quit (end of input does the same)
 *
 * State codes: 0 running, 1 ready, 2 stopped, 3 terminated, 4 blocked
 *
 * Queue design (all singly linked lists, no arrays):
 *   - all_queue:     every process ever admitted, in arrival order, linked through
 *                    global_next. Used by list, PID lookups and exit.
 *   - running_queue: processes currently running (max 3), linked through next.
 *   - ready_queue:   processes waiting for a slot, linked through next.
 *   - blocked_queue: processes waiting for an event, in the order they blocked,
 *                    linked through next (so event unblocks FCFS).
 *   Each Process has two link pointers so it can sit in all_queue and in one state
 *   queue at the same time. Stopped and terminated processes are only in all_queue.
 *
 * Timing: SIGALRM fires every second (1 cycle) to count running time for events;
 * SIGCHLD wakes the main loop when a child exits so its slot is refilled immediately.
 * Both handlers interrupt the blocking fgets (EINTR), after which the main loop reaps
 * dead children, advances cycles/blocks processes, and dispatches from the ready queue.
 */

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
#include <sys/time.h>

typedef struct Process {
    pid_t PID;
    int code;
    char **argv; // program path followed by its arguments, NULL-terminated (passed to execv)
    char *priority;
    int arrival; // admission order, used to break priority ties FCFS (smaller = arrived earlier)
    int event_id; // 1..3 or 0 if none
    int event_at; // number of cycles before blocking event occurs
    int ran; // number of cycles already ran
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

// arrival number given to the next admitted process
int next_arrival = 0;

// process states
const int RUNNING = 0;
const int READY = 1;
const int STOPPED = 2;
const int TERMINATED = 3;
const int BLOCKED = 4;

// max number of processes running at the same time
const int MAX_RUNNING = 3;

// store the number of cycles that have passed
// 1 cycle = 1 second
volatile sig_atomic_t cycles = 0;

// handler for signal when child is done
void on_sigchld(int sig) { /* nothing to do here */ }

// handler for signal when a cycle has passed
void on_sigalrm(int sig) {
    cycles++;
}

// extracts all arguments into an array of strings
// - args: the output array, num_args: the MAX number of arguements for a method
// - returns number of args actually parsed
int extract_args(char* args[], int num_args) {
    int idx = 0;
    char *arg;

    while (idx < num_args && (arg = strtok(NULL, " ")) != NULL) {
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

// finds a process by its pid, returns that process or NULL if not found
Process *find_by_pid(pid_t pid) {
    Process *curr = all_queue->head;

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

// sends a signal to OS process stored in the all_queue
// pid: PID of process, signal: SIGSTOP or SIGKILL
int send_signal_to_process(pid_t pid, int signal) {
    Process *process = find_by_pid(pid);

    if (process == NULL) {
        printf("Invalid PID\n");
        return -1;
    }

    if (signal == SIGSTOP && process->code != RUNNING) {
        printf("Unable to stop a process that is not in the running state (code: 0)\n");
        return -1;
    } else if (signal == SIGKILL && process->code == TERMINATED) {
        printf("Unable to kill a process that has already terminated (code: 3)\n");
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

// prints all processes in the all_queue
void print_global_queue(P_Node_Queue *queue) {
    Process *curr = queue->head;
    printf("%-8s %-6s %s\n", "PID", "STATE", "PRIORITY");
    while (curr != NULL) {
        if (curr->PID != 0) {
            printf("%-8d %-6d %s", curr->PID, curr->code, curr->priority);
        } else {
            printf("%-8c %-6d %s", '-', curr->code, curr->priority); // print '-' when PID is 0 (i.e. process not started yet)
        }

        // blocked processes also show which event they are waiting for
        if (curr->code == BLOCKED) {
            printf(" (waiting for E%d)", curr->event_id);
        }
        printf("\n");
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

// picks the next process to run from the ready queue, returns selected process or NULL if queue is empty
// - highest priority (smallest number) wins
// - ties are broken by original arrival time (FCFS), not by position in the ready queue,
//   so a resumed/unblocked process keeps its place ahead of processes that arrived after it
Process *pick_next_process() {
    Process *best = NULL;
    int best_priority = 0;
    Process *curr = ready_queue->head;

    while (curr != NULL) {
        int curr_priority = parse_priority(curr->priority);
        if (best == NULL || curr_priority < best_priority ||
            (curr_priority == best_priority && curr->arrival < best->arrival)) {
            best = curr;
            best_priority = curr_priority;
        }
        curr = curr->next;
    }

    return best;
}

// start executing a new process, returns 0 on success or -1 if fork failed
int start_process(Process *process) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("Unable to create process");
        return -1;
    } else if (pid == 0) {
        execv(process->argv[0], process->argv);
        perror("Unable to execute program");    // only reached if exec failed
        _exit(1);
    }

    process->PID = pid; // assign actual PID to process object
    return 0;
}

// start/resume a chosen process from the ready queue.
void dispatch() {
    while (running_queue->size < MAX_RUNNING) {
        Process *chosen_one = pick_next_process();
        if (chosen_one == NULL) {
            return;
        }

        remove_from_queue(ready_queue, chosen_one);

        int status;
        if (chosen_one->PID == 0) { // PID = 0 implies a new process
            status = start_process(chosen_one);
        } else { // else it is a process returning from stop or blocked, so just continue it
            status = kill(chosen_one->PID, SIGCONT);
            if (status == -1) {
                perror("Unable to resume process");
            }
        }

        // only occupy a running slot if the process actually started/continued
        if (status == -1) {
            chosen_one->code = TERMINATED;
            continue;
        }

        append_to_queue(running_queue, chosen_one);
        chosen_one->code = RUNNING;
    }
}

// collects child processes that have terminated and removes them from queues
// also sets their status code accordingly.
// runs at the start of the main loop
void handle_dead_children() {
    pid_t dead_child;

    // waitpid: 
    // -1 means wait for any child
    // NULL is just a placeholder to store the status of the child's terminated execution
    // WNOHANG means keep running if no children have finished
    // return PID of child that died
    while ((dead_child = waitpid(-1, NULL, WNOHANG)) > 0) {
        Process *p = find_by_pid(dead_child);
        if (p == NULL) {
            continue;
        }
        p->code = TERMINATED;

        // a child can die while not running (e.g. exits just as it is blocked, or is killed
        // externally while stopped), so remove it from every state queue, not just running
        remove_from_queue(running_queue, p);
        remove_from_queue(ready_queue, p);
        remove_from_queue(blocked_queue, p);
    }
}

// kills every child that has not terminated yet, used by exit and on end of input (Ctrl + D)
void terminate_all() {
    Process *curr = all_queue->head;
    while (curr != NULL) {
        if (curr->PID > 0 && curr->code != TERMINATED) {
            kill(curr->PID, SIGKILL);
            curr->code = TERMINATED;
        }
        curr = curr->global_next;
    }
}

// advances every running process by the cycles that have elapsed since the last call,
// blocking any process whose event has triggered
void handle_cycles() {
    static int last_cycles = 0;

    // the loop also wakes up for SIGCHLD and user input, so only count real elapsed cycles
    int elapsed = cycles - last_cycles;
    last_cycles = cycles;
    if (elapsed <= 0) {
        return;
    }

    Process *curr = running_queue->head;
    while (curr != NULL) {
        Process *next = curr->next; // save now: moving curr between queues overwrites curr->next
        curr->ran += elapsed;

        if (curr->event_at > 0 && curr->ran >= curr->event_at) {
            if (send_signal_to_process(curr->PID, SIGSTOP) == 0) {
                curr->code = BLOCKED;
                remove_from_queue(running_queue, curr);
                append_to_queue(blocked_queue, curr);
            }
        }

        curr = next;
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
    const int RUN_ARGS = 32; // max tokens after "run": program, its arguments, priority, event
    const int RUN_ARGS_MANDATORY = 2; // program and priority
    const int STOP_ARGS = 1;
    const int RESUME_ARGS = 1;
    const int KILL_ARGS = 1;
    const int EVENT_ARGS = 1;

    // set regex comparators
    // for priority and event -> limits input to 9 digits to prevent overflow
    // for pid -> limits input to 7 digits bcos that is the max number a pid can have
    regcomp(&priority_re, "^P[1-9][0-9]{0,8}$", REG_EXTENDED | REG_NOSUB);
    regcomp(&event_re, "^E[1-3]@[1-9][0-9]{0,8}$", REG_EXTENDED | REG_NOSUB);
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
    sigemptyset(&sa.sa_mask); // clear out any garbage bits from memory
    sa.sa_flags = SA_NOCLDSTOP; // ignores stopped processes, only activate when a process is done or killed
    sigaction(SIGCHLD, &sa, NULL);

    // register the handler that activates when a cycle has passed
    struct sigaction sa2 = {0};
    sa2.sa_handler = on_sigalrm;
    sigemptyset(&sa2.sa_mask); // clear out any garbage bits from memory
    sigaction(SIGALRM, &sa2, NULL);

    struct itimerval t = { {1,0}, {1,0} }; // {interval of 1s}, {first instance in 1s}
    setitimer(ITIMER_REAL, &t, NULL); // start the timer, using the above settings. fires SIGALRM each time
    
    // buffer for command input
    char line[256];
    
    // gate for the prompt (cs205$): only print it when the previous one has been consumed by a
    // completed line. If fgets is interrupted by SIGCHLD, the prompt (and whatever the
    // user has typed so far) is still on screen, so we must not print it again.
    bool show_prompt = true;

    while (true) {
        handle_dead_children();
        handle_cycles();
        dispatch();

        if (show_prompt) {
            printf("cs205$ ");
            fflush(stdout);
            show_prompt = false;
        }

        // reset errno first, otherwise a EINTR signal makes a real
        // end of input (Ctrl + D / end of piped script) look like an interruption and loop forever
        errno = 0;
        if (fgets(line, sizeof line, stdin) == NULL) {
            if (errno == EINTR) {
                clearerr(stdin);
                continue;   // interrupted: keep waiting without reprinting the prompt
            }
            terminate_all(); // end of input - terminate all children remaining
            break;
        }

        // only show cs205$ if an actual user input was entered (not an interrupt)
        show_prompt = true;

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
            // layout: args[0] = <program>, args[1..] = <program arguments>,
            //         then <priority>, then optionally <event@cycle> as the last token
            // e.g. run ./prog x60 60 P4 E1@40

            // the event is optional, so work out from the last token where the priority is.
            // a last token containing '@' is treated as an event (validated below)
            char *event = NULL;
            int priority_idx = num_args - 1;
            if (num_args > RUN_ARGS_MANDATORY && strchr(args[num_args - 1], '@') != NULL) {
                event = args[num_args - 1];
                priority_idx = num_args - 2;
            }

            if (num_args < RUN_ARGS_MANDATORY) {
                printf("Usage: run <program> [arguments] <priority> [event@cycle (optional)]\n");
            } else if (num_args == RUN_ARGS && strtok(NULL, " ") != NULL) {
                printf("Too many arguments. At most %d are allowed after run\n", RUN_ARGS);
            } else if (regexec(&priority_re, args[priority_idx], 0, NULL, 0) != 0) {
                printf("Invalid priority. Priority must be in the form P1, P2, P3, ... Pn and come after the program arguments\n");
            } else if (priority_idx == 0) {
                printf("Missing program. Usage: run <program> [arguments] <priority> [event@cycle (optional)]\n");
            } else if (event != NULL && regexec(&event_re, event, 0, NULL, 0) != 0) {
                printf("Invalid event. Event must be in the form E1/E2/E3@<num_cycles_till_event>\n");
            } else if (access(args[0], X_OK) != 0) { 
                // access() checks whether program is able to run or not
                // X_OK -> we have execute permissions
                printf("Unable to run '%s': file does not exist or is not executable\n", args[0]);
            }
            else {
                Process *new_process = malloc(sizeof(Process));
                new_process->PID = 0;
                new_process->code = READY;
                new_process->priority = strdup(args[priority_idx]);
                new_process->arrival = next_arrival++;
                new_process->ran = 0;

                // copy program path + arguments (everything before the priority) into a NULL-terminated argv for execv.
                // priority_idx: it is equivalent to the number of args in the program portion + the program path
                // +1 because of the NULL terminator at the end (for execv)
                new_process->argv = malloc((priority_idx + 1) * sizeof(char *));
                for (int i = 0; i < priority_idx; i++) {
                    new_process->argv[i] = strdup(args[i]);
                }
                new_process->argv[priority_idx] = NULL;

                // handle optional event arg
                if (event != NULL) {
                    int event_id, event_at;
                    sscanf(event, "E%d@%d", &event_id, &event_at);

                    new_process->event_id = event_id;
                    new_process->event_at = event_at;
                } else {
                    new_process->event_id = 0;
                    new_process->event_at = 0;
                }

                append_to_global_queue(all_queue, new_process);

                // new processes go to ready queue first and dispatched soon after
                append_to_queue(ready_queue, new_process); 
            }
        } else if (strcmp(command, STOP_CMD) == 0) {
            char *args[STOP_ARGS + 1];
            int num_args = extract_args(args, STOP_ARGS);

            if (num_args < STOP_ARGS || strtok(NULL, " ") != NULL) {
                printf("Usage: stop <PID>\n");
            } else if (regexec(&pid_re, args[0], 0, NULL, 0) != 0) {
                printf("Invalid PID\n");
            } else {
                pid_t pid = strtol(args[0], NULL, 10);
                if (send_signal_to_process(pid, SIGSTOP) == 0) {
                    printf("stopping %d\n", pid);
                    Process *stopped = find_by_pid(pid);
                    remove_from_queue(running_queue, stopped);
                    stopped->code = STOPPED;
                }
            }
        } else if (strcmp(command, KILL_CMD) == 0) {
            char *args[KILL_ARGS + 1];
            int num_args = extract_args(args, KILL_ARGS);

            if (num_args < KILL_ARGS || strtok(NULL, " ") != NULL) {
                printf("Usage: kill <PID>\n");
            } else if (regexec(&pid_re, args[0], 0, NULL, 0) != 0) {
                printf("Invalid PID\n");
            } else {
                pid_t pid = strtol(args[0], NULL, 10);
                if (send_signal_to_process(pid, SIGKILL) == 0) {
                    printf("terminating %d\n", pid);
                    Process *terminated = find_by_pid(pid);
                    terminated->code = TERMINATED;

                    remove_from_queue(running_queue, terminated);
                    remove_from_queue(blocked_queue, terminated);
                    remove_from_queue(ready_queue, terminated);
                }
            }
        } else if (strcmp(command, RESUME_CMD) == 0) {
            char *args[RESUME_ARGS + 1];
            int num_args = extract_args(args, RESUME_ARGS);

            if (num_args < RESUME_ARGS || strtok(NULL, " ") != NULL) {
                printf("Usage: resume <PID>\n");
            } else if (regexec(&pid_re, args[0], 0, NULL, 0) != 0) {
                printf("Invalid PID\n");
            } else {
                pid_t pid = strtol(args[0], NULL, 10);
                Process *resumed = find_by_pid(pid);
                if (resumed) {
                    if (resumed->code == STOPPED) {
                        printf("resuming %d\n", pid);
                        append_to_queue(ready_queue, resumed);
                        resumed->code = READY;
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
            terminate_all();
            break;
        } else if (strcmp(command, EVENT_CMD) == 0) {
            char *args[EVENT_ARGS + 1];
            int num_args = extract_args(args, EVENT_ARGS);        

            if (num_args < EVENT_ARGS || strtok(NULL, " ") != NULL) {
                printf("Usage: event <E1/E2/E3>\n");
            } else if (strlen(args[0]) != 2 || args[0][0] != 'E' || args[0][1] < '1' || args[0][1] > '3') {
                printf("Invalid event. Event must be E1, E2 or E3\n");
            } else {
                int event_id = args[0][1] - '0';
                int unblocked = 0;

                printf("Event E%d received.\n", event_id);

                // blocked_queue is in the order processes blocked, so walking from the head unblocks FCFS
                Process *curr = blocked_queue->head;

                while (curr != NULL) {
                    Process *next = curr->next; // save now: moving curr between queues overwrites curr->next
                    if (curr->event_id == event_id) {
                        remove_from_queue(blocked_queue, curr);
                        append_to_queue(ready_queue, curr);
                        curr->code = READY;

                        // event has happened, clear it so the process isn't re-blocked next cycle
                        curr->event_id = 0;
                        curr->event_at = 0;

                        printf("Process %d unblocked and moved to Ready Queue.\n", curr->PID);
                        unblocked++;
                    }
                    curr = next;
                }

                if (unblocked == 0) {
                    printf("No processes waiting for E%d.\n", event_id);
                }
            }
        } else {
            printf("Invalid command\n");
        }
    }
}