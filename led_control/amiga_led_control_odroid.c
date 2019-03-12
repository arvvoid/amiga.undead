// I/O activity led control service program for Amiga drive led on odroid
// (can be used for rpi or similar board with minimal changes)
// MIT License
// (C) 2014 arvvoid

#define PIDFILE "/var/run/hddled.pid"

#define VMSTAT "/proc/vmstat"

#define GPIO_INIT "/sys/class/gpio/export"

#define GPIO_INIT_OUT "/sys/class/gpio/gpio199/direction"

#define GPIO_PIN "199\n"

#define GPIO_W "/sys/class/gpio/gpio199/value"

#define _GNU_SOURCE

#include < argp.h >

#include < signal.h >

#include < stdio.h >

#include < stdlib.h >

#include < string.h >

#include < time.h >

#include < unistd.h >

#include < sys/stat.h >

static unsigned int refresh = 20; /* milliseconds */

static int detach = 0;

static volatile sig_atomic_t running = 1;

static char * line = NULL;

static size_t len = 0;

/* Reread the vmstat file */

int activity(FILE * vmstat) {

  static unsigned int prev_pgpgin, prev_pgpgout;

  unsigned int pgpgin, pgpgout;

  int found_pgpgin, found_pgpgout;

  int result;

  /* Reload the vmstat file */

  result = TEMP_FAILURE_RETRY(fseek(vmstat, 0 L, SEEK_SET));

  if (result) {

    perror("Could not rewind " VMSTAT);

    return result;

  }

  /* Clear glibc's buffer */

  result = TEMP_FAILURE_RETRY(fflush(vmstat));

  if (result) {

    perror("Could not flush input stream");

    return result;

  }

  /* Extract the I/O stats */

  found_pgpgin = found_pgpgout = 0;

  while (getline( & line, & len, vmstat) != -1 && errno != EINTR) {

    if (sscanf(line, "pgpgin %u", & pgpgin))

      found_pgpgin++;

    else if (sscanf(line, "pgpgout %u", & pgpgout))

      found_pgpgout++;

    if (found_pgpgin && found_pgpgout)

      break;

  }

  if (!found_pgpgin || !found_pgpgout) {

    fprintf(stderr, "Could not find required lines in " VMSTAT);

    return -1;

  }

  /* Anything changed? */

  result =

    (prev_pgpgin != pgpgin) ||

    (prev_pgpgout != pgpgout);

  prev_pgpgin = pgpgin;

  prev_pgpgout = pgpgout;

  return result;

}

/* Update the LED */

void led(int on) {

  static int current = 1; /* Ensure the LED turns off on first call */

  FILE * gpio = NULL;

  if (current == on)

    return;

  if (on) {

    gpio = fopen(GPIO_W, "w");

    fprintf(gpio, "1\n");

    fclose(gpio);

  } else {

    gpio = fopen(GPIO_W, "w");

    fprintf(gpio, "0\n");

    fclose(gpio);

  }

  current = on;

}

/* Signal handler -- break out of the main loop */

void shutdown(int sig) {

  running = 0;

}

/* Argp parser function */

error_t parse_options(int key, char * arg, struct argp_state * state) {

  switch (key) {

  case 'd':

    detach = 1;

    break;

  case 'r':

    refresh = strtol(arg, NULL, 10);

    if (refresh < 10)

      argp_failure(state, EXIT_FAILURE, 0, "refresh interval must be at least 10");

    break;

  }

  return 0;

}

int main(int argc, char ** argv) {

  struct argp_option options[] = {

    {
      "detach",
      'd',
      NULL,
      0,
      "Detach from terminal - DEAMON mode"
    },

    {
      "refresh",
      'r',
      "VALUE",
      0,
      "Refresh interval (default: 20 ms)"
    },

    {
      0
    },

  };

  struct argp parser = {

    NULL,
    parse_options,
    NULL,

    "Show disk activity using a LED wired to a GPIO pin.",

    NULL,
    NULL,
    NULL

  };

  int status = EXIT_FAILURE;

  FILE * vmstat = NULL;

  FILE * gpio = NULL;

  FILE * pidfile_stream = NULL;

  struct timespec delay;

  /* Parse the command-line */

  parser.options = options;

  if (argp_parse( & parser, argc, argv, ARGP_NO_ARGS, NULL, NULL))

    goto out;

  delay.tv_sec = refresh / 1000;

  delay.tv_nsec = 1000000 * (refresh % 1000);

  struct stat metadata;

  if (stat(PIDFILE, & metadata) == 0) {

    perror("PID file already exists - may already be running");

    goto out;

  } else {

    pidfile_stream = fopen(PIDFILE, "w");

    if (pidfile_stream == NULL) {

      if (errno == EPERM)

        perror("Insufficient permission to create PID file, continuing without a PID file");

      else {

        perror("Can't create PID file: fopen()");

        return EXIT_FAILURE;

      }

    }

  }

  /* Open the vmstat file */

  vmstat = fopen(VMSTAT, "r");

  if (!vmstat) {

    perror("Could not open " VMSTAT " for reading");

    goto out;

  }

  //init the GPIO PIN

  gpio = fopen(GPIO_INIT, "a");

  if (!gpio) {

    perror("Could not open " GPIO_INIT);

    goto out;

  }

  fprintf(gpio, GPIO_PIN);

  fclose(gpio);

  gpio = fopen(GPIO_INIT_OUT, "w");

  if (!gpio) {

    perror("Could not open " GPIO_INIT_OUT);

    goto out;

  }

  fprintf(gpio, "out\n");

  fclose(gpio);

  //OPEN GPIO VALUE FILE

  gpio = fopen(GPIO_W, "w");

  if (!gpio) {

    perror("Could not open " GPIO_INIT_OUT);

    goto out;

  }

  fclose(gpio);

  /* Ensure the LED is off */

  led(0);

  /* Save the current I/O stat values */

  if (activity(vmstat) < 0) {

    goto out;

  }

  /* Detach from terminal? */

  if (detach) {

    pid_t child = fork();

    if (child < 0) {

      perror("Could not detach from terminal");

      goto out;

    }

    if (child) {

      /* I am the parent */

      status = EXIT_SUCCESS;

      goto out2;

    }

    //write the pid file

    if (pidfile_stream) {

      if (fprintf(pidfile_stream, "%d\n", getpid()) < 0) {

        perror("error: writing PID fprintf()");

        goto out;

      } else {

        fclose(pidfile_stream);

      }
    }

    //Change File Mask

    umask(0);

    pid_t sid = setsid();

    if (sid < 0) {

      perror("Failed to get new sid");

      goto out;

    }

    if ((chdir("/")) < 0) {

      perror("Failed chdir /");

      goto out;

    }

    //Close Standard File Descriptors

    close(STDIN_FILENO);

    close(STDOUT_FILENO);

    close(STDERR_FILENO);

  }

  /* We catch these signals so we can clean up */

  {

    struct sigaction action;

    memset( & action, 0, sizeof(action));

    action.sa_handler = shutdown;

    sigemptyset( & action.sa_mask);

    action.sa_flags = 0; /* We block on usleep; don't use SA_RESTART */

    sigaction(SIGHUP, & action, NULL);

    sigaction(SIGINT, & action, NULL);

    sigaction(SIGTERM, & action, NULL);

  }

  /* Loop until signal received */

  while (running) {

    int a;

    if (nanosleep( & delay, NULL) < 0)

      break;

    a = activity(vmstat);

    if (a < 0)

      break;

    led(a);

  }

  /* Ensure the LED is off */

  led(0);

  status = EXIT_SUCCESS;

  out:

    unlink(PIDFILE);

  if (line) free(line);

  out2:

    if (vmstat) fclose(vmstat);

  if (gpio) fclose(gpio);

  return status;

}