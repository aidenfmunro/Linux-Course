#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>
#include <syslog.h>
#include <pthread.h>

#include "file_utils.h"
#include "ipc_utils.h"
#include "config.h"

#define FIFO_PATH "/tmp/backup_daemon_fifo"

static volatile int keep_running = 1;
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;

void handle_signal(int sig) 
{
    switch (sig)
    {
        case SIGHUP:
        {
            syslog(LOG_INFO, "received sighup signal, reloading configuration...");
            pthread_mutex_lock(&config_mutex);

            load_config();

            pthread_mutex_unlock(&config_mutex);
            syslog(LOG_INFO, "configuration reloaded: pid=%d, interval=%dms", monitored_pid, sampling_interval);
        }

        break;

        case SIGINT:
        {
            keep_running = 1;
        }

        break;

        case SIGTERM:
        {
            keep_running = 0;
        }

        break;
        
        case SIGKILL:
        {
            keep_running = 0;
        }

        break;
    }
}

void daemonize()
{
    pid_t pid = fork();

    if (pid < 0) exit(EXIT_FAILURE);

    if (pid > 0) exit(EXIT_SUCCESS);

    if (setsid() < 0) exit(EXIT_FAILURE);

    pid = fork();

    if (pid < 0) exit(EXIT_FAILURE);

    if (pid > 0) exit(EXIT_SUCCESS);

    umask(0);

    chdir("/");

    for (int fd = sysconf(_SC_OPEN_MAX); fd >= 0; --fd)
    {
        close(fd);
    }

    int null_fd = open("/dev/null", O_RDWR);

    if (null_fd != -1) 
    {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
    }

    close(null_fd);
}

// Monitoring function (uses mutex for safe access to globals)
void monitor_process() 
{
    char cwd_path[256];
    char real_path[PATH_MAX];

    while (keep_running) 
    {
        pthread_mutex_lock(&config_mutex);
        int pid = monitored_pid;
        int interval = sampling_interval;
        pthread_mutex_unlock(&config_mutex);

        snprintf(cwd_path, sizeof(cwd_path), "/proc/%d/cwd", pid);

        if (readlink(cwd_path, real_path, sizeof(real_path)) == -1) 
        {
            syslog(LOG_ERR, "Failed to read the current working directory of pid %d", pid);
            break;
        }

        syslog(LOG_INFO, "Monitoring directory: %s", real_path);

        perform_backup(real_path);

        usleep(interval * 1000);
    }
}

int main(int argc, char *argv[]) 
{
    int daemon_mode = 0;

    int opt;
    int pid_argument = 0;

    while ((opt = getopt(argc, argv, "dp:")) != -1) 
    {
        switch (opt) 
        {
        case 'd':
            daemonize();
            break;
        case 'p':
            pid_argument = atoi(optarg);

            if (pid_argument <= 0) 
            {
                fprintf(stderr, "Invalid PID: %s\n", optarg);
                exit(EXIT_FAILURE);
            }

            monitored_pid = pid_argument;
            printf("PID set to %d\n", monitored_pid);
            break;
        default:
            fprintf(stderr, "Usage: %s [-d] [-p pid]\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    save_config();

    if (monitored_pid <= 0) 
    {
        fprintf(stderr, "Invalid monitored PID in configuration file.\n");
        exit(EXIT_FAILURE);
    }

    if (sampling_interval <= 0) 
    {
        fprintf(stderr, "Invalid sampling interval in configuration file.\n");
        exit(EXIT_FAILURE);
    }

    if (daemon_mode) daemonize();

    signal(SIGHUP, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Open syslog
    openlog("backupd", LOG_PID | LOG_CONS, LOG_DAEMON);

    syslog(LOG_INFO, "Daemon started. Monitoring PID: %d, Interval: %dms", monitored_pid, sampling_interval);

    // Set up IPC channel
    setup_ipc(FIFO_PATH);

    // Start monitoring process
    monitor_process();

    syslog(LOG_INFO, "Daemon shutting down.");
    cleanup_ipc(FIFO_PATH);

    // Close syslog
    closelog();
    return 0;
}
