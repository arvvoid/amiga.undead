/*
 * HDD Activity LED Control Service
 *
 * Description:
 *   This program monitors disk I/O activity on an Odroid system and controls an LED connected to
 *   a GPIO pin, emulating the classic Amiga drive LED behavior. With minimal modifications,
 *   it can be adapted for use on Raspberry Pi or similar single-board computers.
 *
 * Features:
 *   - Monitors disk activity by reading from /proc/vmstat.
 *   - Controls an LED via a specified GPIO pin based on detected disk activity.
 *   - Supports running as a daemon with optional refresh interval configuration.
 *   - Handles signals for graceful shutdown and resource cleanup.
 *
 * Usage:
 *   - Compile the program using a C compiler (`zig cc` can be used as well).
 *   - Run the executable with appropriate permissions (may require root).
 *   - Use command-line options to customize behavior (see --help for details).
 *
 * Command-line Options:
 *   --detach, -d           Run as a daemon (detach from terminal).
 *   --refresh, -r VALUE    Set the refresh interval in milliseconds (default: 20 ms).
 *
 * License:
 *   MIT License
 *
 * Author:
 *   arvvoid (C) 2014-2024
 *
 *
 * Notes:
 *   - Ensure the GPIO pin number and paths are correct for your hardware setup.
 *   - The program may require root privileges to access GPIO and system files.
 *   - Modify the GPIO paths and pin numbers as needed for different boards.
 */

#define _GNU_SOURCE  // Required for getline()

#include <argp.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define PIDFILE "/var/run/hddled.pid"
#define VMSTAT_PATH "/proc/vmstat"
#define GPIO_EXPORT_PATH "/sys/class/gpio/export"
#define GPIO_DIRECTION_PATH "/sys/class/gpio/gpio199/direction"
#define GPIO_VALUE_PATH "/sys/class/gpio/gpio199/value"
#define GPIO_PIN_NUMBER "199"

static unsigned int refresh_interval_ms = 20;
static int run_as_daemon = 0;
static volatile sig_atomic_t running = 1;

/* Signal handler to gracefully exit the main loop */
void shutdown_handler(int sig) {
    running = 0;
}

/* Function to parse command-line options */
static error_t parse_option(int key, char *arg, struct argp_state *state) {
    switch (key) {
        case 'd':
            run_as_daemon = 1;
            break;
        case 'r':
            refresh_interval_ms = atoi(arg);
            if (refresh_interval_ms < 10)
                argp_error(state, "Refresh interval must be at least 10 milliseconds");
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

/* Function to check disk activity by reading /proc/vmstat */
static int check_disk_activity(FILE *vmstat_file) {
    static unsigned int prev_pgpgin = 0, prev_pgpgout = 0;
    unsigned int pgpgin = 0, pgpgout = 0;
    int found_pgpgin = 0, found_pgpgout = 0;
    char *line = NULL;
    size_t len = 0;

    // Rewind the vmstat file to read from the beginning
    if (fseek(vmstat_file, 0L, SEEK_SET) != 0) {
        perror("Failed to rewind " VMSTAT_PATH);
        return -1;
    }
    clearerr(vmstat_file);

    // Read vmstat line by line
    while (getline(&line, &len, vmstat_file) != -1) {
        if (sscanf(line, "pgpgin %u", &pgpgin) == 1)
            found_pgpgin = 1;
        else if (sscanf(line, "pgpgout %u", &pgpgout) == 1)
            found_pgpgout = 1;

        if (found_pgpgin && found_pgpgout)
            break;
    }

    free(line);

    if (!found_pgpgin || !found_pgpgout) {
        fprintf(stderr, "Failed to find pgpgin and pgpgout in " VMSTAT_PATH "\n");
        return -1;
    }

    // Determine if disk activity has occurred
    int activity_detected = (prev_pgpgin != pgpgin) || (prev_pgpgout != pgpgout);
    prev_pgpgin = pgpgin;
    prev_pgpgout = pgpgout;

    return activity_detected;
}

/* Function to control the LED state */
void control_led(int state, FILE *gpio_value_file) {
    static int current_state = -1;  // Ensure the first write occurs
    if (current_state == state)
        return;

    // Rewind the file to overwrite the value
    if (fseek(gpio_value_file, 0L, SEEK_SET) != 0) {
        perror("Failed to rewind GPIO value file");
        return;
    }

    if (fprintf(gpio_value_file, "%d\n", state) < 0) {
        perror("Failed to write to GPIO value file");
        return;
    }

    fflush(gpio_value_file);  // Ensure the value is written immediately
    current_state = state;
}

int main(int argc, char **argv) {
    int status = EXIT_FAILURE;
    FILE *vmstat_file = NULL;
    FILE *gpio_value_file = NULL;
    FILE *pid_file = NULL;
    struct timespec delay;

    // Command-line argument parsing setup
    const char *argp_program_version = "hddled 1.0";
    static struct argp_option options[] = {
        {"detach", 'd', 0, 0, "Run as daemon (detach from terminal)"},
        {"refresh", 'r', "MILLISECONDS", 0, "Refresh interval in milliseconds (default: 20)"},
        {0}
    };
    static struct argp argp = {options, parse_option, 0, "Show disk activity using a GPIO-connected LED."};

    // Parse command-line arguments
    argp_parse(&argp, argc, argv, 0, 0, 0);

    // Set the refresh interval
    delay.tv_sec = refresh_interval_ms / 1000;
    delay.tv_nsec = (refresh_interval_ms % 1000) * 1000000L;

    // Check for existing PID file
    if (access(PIDFILE, F_OK) == 0) {
        fprintf(stderr, "PID file %s already exists. Is the program already running?\n", PIDFILE);
        goto cleanup;
    }

    // Open vmstat file
    vmstat_file = fopen(VMSTAT_PATH, "r");
    if (!vmstat_file) {
        perror("Failed to open " VMSTAT_PATH " for reading");
        goto cleanup;
    }

    // Initialize GPIO pin
    FILE *gpio_file = fopen(GPIO_EXPORT_PATH, "w");
    if (!gpio_file) {
        perror("Failed to open " GPIO_EXPORT_PATH " for writing");
        goto cleanup;
    }
    fprintf(gpio_file, GPIO_PIN_NUMBER "\n");
    fclose(gpio_file);

    // Set GPIO direction to output
    gpio_file = fopen(GPIO_DIRECTION_PATH, "w");
    if (!gpio_file) {
        perror("Failed to open " GPIO_DIRECTION_PATH " for writing");
        goto cleanup;
    }
    fprintf(gpio_file, "out\n");
    fclose(gpio_file);

    // Open GPIO value file
    gpio_value_file = fopen(GPIO_VALUE_PATH, "w");
    if (!gpio_value_file) {
        perror("Failed to open " GPIO_VALUE_PATH " for writing");
        goto cleanup;
    }

    // Ensure the LED is off
    control_led(0, gpio_value_file);

    // Initialize previous activity values
    if (check_disk_activity(vmstat_file) < 0)
        goto cleanup;

    // Run as daemon if requested
    if (run_as_daemon) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("Failed to fork");
            goto cleanup;
        }
        if (pid > 0) {
            // Parent process exits
            status = EXIT_SUCCESS;
            goto cleanup;
        }

        // Child process continues
        if (setsid() < 0) {
            perror("Failed to create new session");
            goto cleanup;
        }
        if (chdir("/") < 0) {
            perror("Failed to change directory to /");
            goto cleanup;
        }
        // Redirect standard file descriptors to /dev/null
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
    }

    // Create PID file
    pid_file = fopen(PIDFILE, "w");
    if (!pid_file) {
        perror("Failed to create PID file");
        goto cleanup;
    }
    fprintf(pid_file, "%d\n", getpid());
    fclose(pid_file);

    // Set up signal handlers
    struct sigaction sa;
    sa.sa_handler = shutdown_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Main loop
    while (running) {
        if (nanosleep(&delay, NULL) < 0 && errno != EINTR) {
            perror("nanosleep failed");
            break;
        }

        int activity_detected = check_disk_activity(vmstat_file);
        if (activity_detected < 0)
            break;

        control_led(activity_detected, gpio_value_file);
    }

    // Ensure the LED is off before exiting
    control_led(0, gpio_value_file);
    status = EXIT_SUCCESS;

cleanup:
    if (gpio_value_file)
        fclose(gpio_value_file);
    if (vmstat_file)
        fclose(vmstat_file);
    if (pid_file)
        fclose(pid_file);
    unlink(PIDFILE);

    return status;
}
