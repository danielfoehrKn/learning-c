#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <libc.h>
#include <errno.h>
#include <util.h>

// written for OS X
// needs changes to the includes to work on Linux
int main() {
    // forked process inherits all open file descriptors from parent
    pid_t childProcessID = fork();

    pid_t pid = getpid();
    pid_t ppid = getppid();
    gid_t processGroupID = getpgrp();
    pid_t sessionID = getsid(pid);

    bool isProcessGroupLeader;
    if (pid == processGroupID) {
        isProcessGroupLeader = true;
    }

    bool isSessionLeader;
    if (pid == sessionID) {
        isSessionLeader = true;
    }

    if (childProcessID == 0) {
        // sleep to let the parent finish earlier to get adopted by the init process
        sleep(1);
        // has inherited duplicates of all open fds from parent process
        // even though parent exited already, still has STDOUT filestream open to STDIN  file descriptor of TTY (my terminal that started it)
        // printf prints to STDOUT
        printf("I am a child with pID %d and ppID %d and groupID %d and sessionID %d. ProcessGroupLeader: %d. SessionLeader: %d \n", pid, ppid, processGroupID, sessionID, isProcessGroupLeader, isSessionLeader);

        // demonstrates that a fork / child process in the same session can open (NOT create it via openpty())
        // the controlling terminal TTY device
        int ttyFileDescriptor = open("/dev/tty", O_WRONLY);
        if (ttyFileDescriptor == -1) {
            // besides opening a file and writing logs, cannot do anything here as STDOUT is not open
            printf("child failed to open /dev/tty: %d", errno);
            exit(EXIT_FAILURE);
        }

        // closing STDOUT to demonstrate dup copying the file descriptor from the TTY to position 1
        if (close(STDOUT_FILENO) == -1) {
            printf("failed to close STDOUT. Last error code: %d", errno);
            exit(EXIT_FAILURE);
        }

        // dprintf does not print to filedescriptor 1 STDIN but it is configurable
        // print to newly acquired TTY fd
        dprintf(ttyFileDescriptor, "I am a child with pID %d and opened the TTY file descriptor: %d \n", pid, ttyFileDescriptor);

        // duplicate TTY file descriptor again to STDOUT,
        // otherwise following printf will not reach the terminal
        int dubbed = dup2(ttyFileDescriptor, STDOUT_FILENO);
        if (dubbed == -1) {
            dprintf(ttyFileDescriptor, "I am a child with pID %d and failed to dup2() the TTY file descriptor to STDOUT. Errno: %d :( \n", pid, errno);
            exit(EXIT_FAILURE);
        }

        // close tty file descriptor 3 again
        if (close(ttyFileDescriptor) == -1) {
            printf("failed to close the TTY file descriptor again. Last error code: %d \n", errno);
            exit(EXIT_FAILURE);
        }

        // create log file because STDOUT and STDERR will be set to dev/null
        // the new session leader could obtain an new controlling terminal it again via openpty(),
        // but we do not want that
        int logfile_fileno = open("daemon.log",O_RDWR|O_CREAT|O_APPEND,S_IRUSR|S_IWUSR|S_IRGRP);
        if (logfile_fileno == -1) {
            printf("failed to open logfile (errno=%d)",errno);
        }

        // Setting child's 0,1,2 fds to dev/null
        // first closing them, then open starting from the lowest number (0 - 2)
        // do it before setid() to have ability to write to controlling terminal (see output)
        if (close(STDIN_FILENO) == -1) {
            printf("failed to close STDIN. Last error code: %d", errno);
            exit(EXIT_FAILURE);
        }

        if (close(STDERR_FILENO) == -1) {
            printf("failed to close STDERR. Last error code: %d", errno);
            exit(EXIT_FAILURE);
        }

        if (close(STDOUT_FILENO) == -1) {
            dprintf(3, "failed to close STDOUT. Last error code: %d", errno);
            exit(EXIT_FAILURE);
        }

        // set STDIN to dev/null
        if (open("/dev/null",O_RDONLY) == -1) {
            exit(EXIT_FAILURE);
        }

        // set STDOUT to dev/null
        if (open("/dev/null",O_WRONLY) == -1) {
            exit(EXIT_FAILURE);
        }

        // set STDERR to dev/null
        if (open("/dev/null",O_RDWR) == -1) {
            exit(EXIT_FAILURE);
        }

        // We want to create a daemon process like the containerd-runtime-shim!
        // 1) Create a new session to remove the ability of the child to re-acquire a controlling terminal
        //    Child process becomes session leader (could theoretically acquire new controlling terminal)
        //    Also see: https://www.gnu.org/software/libc/manual/html_node/Controlling-Terminal.html
        // 2) Close all STDIO file descriptors via close() and then set to dev/null device
        //    This device is always there. If fds would be closed, writing to e.g STDOUT would fail and could cause problems
        //      See: https://linux.die.net/man/2/close
        // 3) Child forks to create the daemon
        // 4) Daemon
        //     - is no session leader (no way to acquire terminal!)
        //     - Daemon has inherited 0,1,2 fds set to dev/null and fd 4 to the log file

        pid_t newSID = setsid();
        if (newSID == -1) {
            dprintf(3, "child with pid %d failed to create a new session. Last error code: %d \n", pid, errno);
            exit(EXIT_FAILURE);
        };


        // do not have access to the controlling terminal of the old session any more
        // this printf will not be printed
        dprintf(3, "This will be logged!: %d \n", newSID);

        pid_t childNewProcessGroupID = getpgrp();
        pid_t childNewSessionID = getsid(pid);
        dprintf(3, "I am a child in a new session with pid %d and ppid %d and groupID %d and sessionID %d. ProcessGroupLeader: %d. SessionLeader: %d \n",
                pid,
                ppid,
                childNewProcessGroupID,
                childNewSessionID,
                pid == childNewProcessGroupID,
                pid == childNewSessionID);

        // now all the file descriptors are closed and session is disconnected
        // from controlling terminal of previous session
        // ready to creat another fork (double fork) and terminate the child
        pid_t daemonProcessID = fork();

        // wait to observe child an daemon
        sleep(6);

        // This special file is a synonym within the kernel for the EXISTING controlling terminal of the current process.
        // Naturally, if the program doesn't have a controlling terminal, the open of this device will fail.
        // this is a new session, so there is no controlling terminal
        ttyFileDescriptor = open("/dev/tty", O_WRONLY);
        if (ttyFileDescriptor == 0) {
            dprintf(3, "Process %d was able to obtain the current terminal, this is not possible as it is in a new session \n", getpid());
            exit(EXIT_FAILURE);
        }

        // exit the child process to complete the double fork to create the daemon
        // parent finished to wait child exit code and daemon will keep on running independently
        if (daemonProcessID != 0 ) {
            dprintf(3, "Forked the Daemon. Child over and out. \n");
            exit(EXIT_SUCCESS);
        }

        // Set the user file creation mask to zero.
        umask(0);

        pid_t daemonPID = getpid();
        pid_t daemonProcessGroupID = getpgrp();
        dprintf(3, "I am a daemon process with id %d and ppID %d and groupID %d and sessionID %d. ProcessGroupLeader: %d. SessionLeader: %d \n",
                daemonPID,
                getppid(),
                daemonProcessGroupID,
                getsid(daemonPID),
                daemonPID == daemonProcessGroupID,
                daemonPID == getsid(daemonPID));

        // Demonstrate that the daemon process CANNOT obtain a controlling terminal
        // Only session leaders can obtain a controlling terminal via openpty()
        // https://man7.org/linux/man-pages/man3/openpty.3.html

        // TODO: somehow the daemon can acquire a PTY
        // not sure of that is OSX specific behaviour, but on Linux that should not work
        int masterFd, slaveFd;
        if (openpty(&masterFd, &slaveFd, NULL, NULL, NULL) == -1) {
            dprintf(3, "I am a daemon process with id %d and failed to obtain a controlling terminal. This is expected. \n", getpid());
        } else {
            dprintf(3, "I am a daemon process with id %d and was able to obtain a controlling terminal. This is UNEXPECTED. \n", getpid());
        }

        // In container runtimes, the daemon first creates a Unix Domain socket and listens on it
        // Then it fork() & exec() the OIC runtime handing over the open file descriptor to the UDS socket
        // The OCI runtime creates a news session &  then is able to obtain a PTY via openpty() and send the file descriptor
        // of the master side over the UDS (slave is STDIO for the container process)
        // The daemon receives the master end of the PTY by listening on  the Unix Domain Socket
        // then calls grantpt() & unlockpt() --> after the container process can write to the PTY slave side
        // the container process has STDIO set to the PTY slave
        // TODO: create sub process from the daemon and hold the PTY master side (get the master fd send via unix domain socket like containerd is doing it)

    } else {
        printf("I am a parent with pID %d and ppID %d and groupID %d and sessionID %d. ProcessGroupLeader: %d. SessionLeader: %d \n", pid, ppid, processGroupID, sessionID, isProcessGroupLeader, isSessionLeader);
        // wait for any child to finish (wait for process signal)
        // child will use setsId(), fork() the daemon and the exit itself to complete the double fork
        if (wait(NULL) == -1) {
            printf("parent failed to wait for child process %d \n", childProcessID);
            exit(EXIT_FAILURE);
        }
        printf("Parent successfully waited for child process %d \n", childProcessID);
    }
    
    if (ppid == 1) {
        printf("I am %d and I got adopted by the init process with pid 1 \n", getpid());
    }

    // fileno(openFile) gets the file descriptor (per-process number) of a c filestream
    // stdout file descriptor already opened by kernel
    // this is always 2 in this case
    int stdoutFileDescriptor = fileno(stdout);
    // checks if the file descriptor points to a TTY special device (see files and device types in UNIX)
    if (isatty(stdoutFileDescriptor))  {
        printf("I am %d and stdout is a TTY! \n", getpid());
    } else {
        printf("I am %d and stdout is NOT a TTY. \n", getpid());
    }

    return 0;
}
