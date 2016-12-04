/* ptwrap.c: a simple tool that runs a command in a pseudo-terminal */
/*
MIT License

Copyright (c) 2016 WATANABE Yuki

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#define _XOPEN_SOURCE 600
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static const char *program_name;

static void error_exit(const char *message) {
    fprintf(stderr, "%s: %s\n", program_name,  message);
    exit(EXIT_FAILURE);
}

static void errno_exit(const char *message) {
    fprintf(stderr, "%s: ", program_name);
    perror(message);
    exit(EXIT_FAILURE);
}

static volatile sig_atomic_t should_set_terminal_size = false;
static sigset_t mask_for_select;

static void receive_sigwinch(int sigwinch) {
    should_set_terminal_size = true;
}

static void install_sigwinch_handler(void) {
#if defined(SIGWINCH)
    struct sigaction action;

    /* Block SIGWINCH and prepare mask_for_select */
    if (sigemptyset(&action.sa_mask) < 0 || sigemptyset(&mask_for_select) < 0)
        errno_exit("sigemptyset");
    if (sigaddset(&action.sa_mask, SIGWINCH) < 0)
        errno_exit("sigaddset");
    if (sigprocmask(SIG_BLOCK, &action.sa_mask, &mask_for_select) < 0)
        errno_exit("sigprocmask");
    if (sigdelset(&mask_for_select, SIGWINCH) < 0)
        errno_exit("sigdelset");

    /* Set signal handler for SIGWINCH */
    action.sa_flags = 0;
    action.sa_handler = receive_sigwinch;
    if (sigaction(SIGWINCH, &action, NULL) < 0)
        errno_exit("sigaction");
#endif /* defined(SIGWINCH) */
}

static void set_terminal_size(int fd) {
    should_set_terminal_size = false;
#if defined(TIOCGWINSZ) && defined(TIOCSWINSZ)
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) >= 0)
        ioctl(fd, TIOCSWINSZ, &size);
#endif /* defined(TIOCGWINSZ) && defined(TIOCSWINSZ) */
}

static int prepare_master_pseudo_terminal(void) {
    int fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (fd < 0)
        errno_exit("cannot open master pseudo-terminal");
    if (fd <= STDERR_FILENO)
        error_exit("stdin/stdout/stderr are not open");

    if (grantpt(fd) < 0)
        errno_exit("pseudo-terminal permission not granted");
    if (unlockpt(fd) < 0)
        errno_exit("pseudo-terminal permission not unlocked");

    set_terminal_size(fd);

    return fd;
}

static const char *slave_pseudo_terminal_name(int master_fd) {
    errno = 0; /* ptsname may not assign to errno, even if on error */
    const char *name = ptsname(master_fd);
    if (name == NULL)
        errno_exit("cannot name slave pseudo-terminal");
    return name;
}

static struct termios original_termios;

static bool disable_canonical_io(void) {
    if (tcgetattr(STDIN_FILENO, &original_termios) < 0)
        return false;

    struct termios new_termios = original_termios;
    new_termios.c_iflag &=
            ~(BRKINT | ICRNL | IGNBRK | IGNCR | INLCR | IXON | IXOFF | PARMRK);
    new_termios.c_oflag &= ~OPOST;
    new_termios.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    new_termios.c_cc[VMIN] = 1;
    new_termios.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSADRAIN, &new_termios) >= 0;
}

static void enable_canonical_io(void) {
    tcsetattr(STDIN_FILENO, TCSADRAIN, &original_termios);
    /* TODO: Call tcgetattr again and warn if the original termios has not been
     * restored. */
}

enum state_T { INACTIVE, READING, WRITING, };
struct channel_T {
    int from_fd, to_fd;
    enum state_T state;
    char buffer[BUFSIZ];
    size_t buffer_position, buffer_length;
};

static void set_fd_set(
        struct channel_T *channel, fd_set *read_fds, fd_set *write_fds) {
    switch (channel->state) {
    case INACTIVE: break;
    case READING:  FD_SET(channel->from_fd, read_fds); break;
    case WRITING:  FD_SET(channel->to_fd, write_fds);  break;
    }
}

static void process_buffer(
        struct channel_T *channel, fd_set *read_fds, fd_set *write_fds) {
    ssize_t size;
    switch (channel->state) {
    case INACTIVE:
        break;
    case READING:
        if (!FD_ISSET(channel->from_fd, read_fds))
            break;
        channel->buffer_position = 0;
        size = read(channel->from_fd, channel->buffer, BUFSIZ);
        if (size <= 0) {
            channel->state = INACTIVE;
        } else {
            channel->state = WRITING;
            channel->buffer_length = size;
        }
        break;
    case WRITING:
        if (!FD_ISSET(channel->to_fd, write_fds))
            break;
        assert(channel->buffer_position < channel->buffer_length);
        size = write(channel->to_fd,
                &channel->buffer[channel->buffer_position],
                channel->buffer_length - channel->buffer_position);
        if (size < 0)
            break; /* ignore any error */
        channel->buffer_position += size;
        if (channel->buffer_position == channel->buffer_length)
            channel->state = READING;
        break;
    }
}

static void forward_all_io(int master_fd) {
    struct channel_T incoming, outgoing;
    incoming.from_fd = STDIN_FILENO;
    incoming.to_fd = master_fd;
    outgoing.from_fd = master_fd;
    outgoing.to_fd = STDOUT_FILENO;
    incoming.state = outgoing.state = READING;

#if defined(SIGWINCH)
    const sigset_t *mask = &mask_for_select;
#else /* defined(SIGWINCH) */
    const sigset_t *mask = NULL;
#endif /* defined(SIGWINCH) */

    /* Loop until all output from the slave are forwarded, so that the don't
     * miss any output. On the other hand, we don't know exactly how much
     * input should be forwarded. */
    while (/* incoming.state != INACTIVE || */ outgoing.state != INACTIVE) {
        /* await next IO */
        fd_set read_fds, write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        set_fd_set(&incoming, &read_fds, &write_fds);
        set_fd_set(&outgoing, &read_fds, &write_fds);
        if (pselect(master_fd + 1, &read_fds, &write_fds, NULL,
                    NULL, mask) < 0) {
            if (errno != EINTR)
                /* XXX: Exiting here will leave stdin in non-canonical mode! */
                errno_exit("cannot find file descriptor to forward");
            if (should_set_terminal_size)
                set_terminal_size(master_fd);
            continue;
        }

        /* read to or write from buffer */
        process_buffer(&incoming, &read_fds, &write_fds);
        process_buffer(&outgoing, &read_fds, &write_fds);
    }
}

static int await_child(pid_t child_pid) {
    int wait_status;
    if (waitpid(child_pid, &wait_status, 0) != child_pid)
        /* XXX: Exiting here will leave stdin in non-canonical mode! */
        errno_exit("cannot await child process");
    if (WIFEXITED(wait_status))
        return WEXITSTATUS(wait_status);
    if (WIFSIGNALED(wait_status))
        return WTERMSIG(wait_status) | 0x80;
    return EXIT_FAILURE;
}

static void become_session_leader(void) {
    if (setsid() < 0)
        errno_exit("cannot create new session");
}

static void prepare_slave_pseudo_terminal_fds(const char *slave_name) {
    if (close(STDIN_FILENO) < 0)
        errno_exit("cannot close old stdin");
    int slave_fd = open(slave_name, O_RDWR);
    if (slave_fd != STDIN_FILENO)
        errno_exit("cannot open slave pseudo-terminal at stdin");

    if (close(STDOUT_FILENO) < 0)
        errno_exit("cannot close old stdout");
    if (dup(slave_fd) != STDOUT_FILENO)
        errno_exit("cannot open slave pseudo-terminal at stdout");

    if (close(STDERR_FILENO) < 0)
        errno_exit("cannot close old stderr");
    if (dup(slave_fd) != STDERR_FILENO)
        errno_exit("cannot open slave pseudo-terminal at stderr");

    /* How to become the controlling process of a slave pseudo-terminal is
     * implementation-dependent. We assume Linux-like behavior where a process
     * automatically acquires a controlling terminal in the "open" system call.
     * There is a race condition in this scheme: an unrelated process could
     * open the terminal before we do, in which case the slave is not our
     * controlling terminal and therefore we should abort. We do not support
     * other implementation where a controlling terminal cannot be acquired
     * just by opening a terminal. */
    if (tcgetpgrp(slave_fd) != getpgrp())
        error_exit(
                "cannot become controlling process of slave pseudo-terminal");
}

static void exec_command(char *argv[]) {
    execvp(argv[0], argv);
    errno_exit(argv[0]);
}

int main(int argc, char *argv[]) {
    if (argc <= 0)
        exit(EXIT_FAILURE);
    program_name = argv[0];

    /* Don't use getopt, because we don't want glibc's reordering extension.
    if (getopt(argc, argv, "") != -1)
        exit(EXIT_FAILURE);
    */
    optind = 1;
    if (optind < argc && strcmp(argv[optind], "--") == 0)
        optind++;

    if (optind == argc)
        error_exit("operand missing");

    install_sigwinch_handler();

    int master_fd = prepare_master_pseudo_terminal();
    const char *slave_name = slave_pseudo_terminal_name(master_fd);

    pid_t child_pid = fork();
    if (child_pid < 0)
        errno_exit("cannot spawn child process");
    if (child_pid > 0) {
        /* parent process */
        bool noncanon = disable_canonical_io();
        forward_all_io(master_fd);
        int exit_status = await_child(child_pid);
        if (noncanon)
            enable_canonical_io();
        return exit_status;
    } else {
        /* child process */
        close(master_fd);
        become_session_leader();
        prepare_slave_pseudo_terminal_fds(slave_name);
        exec_command(&argv[optind]);
    }
}

/* vim: set et sw=4 sts=4 tw=79: */
