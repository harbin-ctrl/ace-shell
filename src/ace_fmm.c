/*
 * The ACE fmm: the only part of ACE that is root.
 *
 * It has no HOME, no current directory of consequence, no configuration, no
 * GUI, no D-Bus connection, and no way to be asked to run a program.  Those
 * absences are the design, not an oversight.  Everything a person interacts
 * with -- the shell, the console, the commands, the broker -- stays the
 * user's own process, because a Unix session bus peer and the ownership of a
 * configuration directory follow a process's identity and cannot be borrowed.
 *
 * What arrives here is a typed request naming an operation and an object.
 * What goes back is a status.  This file's whole job is to keep that true.
 *
 * This process is the root supervisor.  It is the only root peer the broker
 * ever authenticates, and it performs no privileged operation itself.  It
 * authenticates once, creates the private mount namespace its children live
 * in, and then owns two kinds of child:
 *
 *   - one volume worker, which holds the mounts and nothing else;
 *   - one access worker (CRM) per broker request, which opens protected files
 *     and cannot mount anything.
 *
 * Its whole remaining job is routing: volume-class records go down the
 * private channel to the volume worker and nowhere else, access-class work
 * happens on a channel the broker holds directly, and neither child has a
 * descriptor reaching the other.  It translates no pathname, mounts nothing,
 * opens nothing, and keeps no state beyond child pids, their channels, and
 * the one capability string the volume worker reports back.
 *
 * The namespace is created here rather than in the volume worker because
 * fork() shares a mount namespace and unshare() does not: creating it once,
 * before either child exists, is what lets an access worker see the device
 * view the volume worker mounts.  Creating a container is not mount policy --
 * every decision about what may be mounted, and where, stays in the volume
 * worker, which is the only process here that calls mount() at all.
 */

#define _GNU_SOURCE

#include "ace_privilege_protocol.h"
#include "ace_crm.h"
#include "ace_fmm_volume.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/un.h>
#include <unistd.h>

struct fmm_state {
    int fd;
    /* The private channel to the volume worker, or -1 in a process that is
       the volume worker (and so has nothing to forward to) or in a supervisor
       whose worker has died. */
    int volume_fd;
    pid_t volume_pid;
    /* Set only in the volume worker itself: it serves volume requests rather
       than passing them on.  The same dispatch code runs on both sides of the
       private channel, and this is the one bit that differs. */
    int serves_volume;
    /*
     * The device view root, as the volume worker reported it.
     *
     * Remembered rather than derived.  The supervisor no longer runs the code
     * that chooses this path, and a second process reconstructing it from the
     * uid and the environment would be guessing at a capability -- right
     * until the day the two guesses diverged.  It arrives in the reply to
     * PREPARE_VIEW, which is the message whose whole purpose is to say where
     * the view lives, and it is handed to access workers unchanged.
     */
    char view_root[PATH_MAX];
    uid_t served_uid;
    pid_t broker_pid;
    uint32_t capabilities;
    uint32_t authorisation_seconds;
    /* Wall-clock point at which authorisation lapses, or zero for "until the
       session ends", which is the default and the normal case. */
    long long deadline_ms;
};

static long long now_ms(void)
{
    struct timespec instant;

    clock_gettime(CLOCK_MONOTONIC, &instant);
    return (long long)instant.tv_sec * 1000 + instant.tv_nsec / 1000000;
}

/*
 * Which user this fmm serves.
 *
 * pkexec publishes the invoking uid, and that is the authority: it is the
 * identity polkit actually authorised, not something the broker told us.  The
 * fallbacks matter only for a fmm started some other way -- a test, or a
 * development run -- and in that case serving the uid we are already running
 * as is the honest answer.
 */
static uid_t resolve_served_uid(void)
{
    const char *value = getenv("PKEXEC_UID");

    if (!value || !*value)
        value = getenv("SUDO_UID");
    if (value && *value) {
        char *end = NULL;
        unsigned long parsed = strtoul(value, &end, 10);

        if (end && *end == '\0')
            return (uid_t)parsed;
    }
    return getuid();
}

/*
 * Recover the nonce from the socket path we were launched with.
 *
 * The broker will prove it knows this same value.  What that establishes is
 * narrow and worth stating exactly: the peer is a process that could read a
 * name inside a directory only the served user may enter.  It is not, on its
 * own, proof of identity -- SO_PEERCRED below is what supplies that -- but it
 * closes the case of a fmm being pointed at a channel it was not started
 * for.
 */
static int nonce_from_path(const char *path, uint8_t *nonce)
{
    const char *name = strrchr(path, '/');
    const char *prefix = "fmm-";
    size_t index;

    name = name ? name + 1 : path;
    if (strncmp(name, prefix, strlen(prefix)) != 0)
        return -1;
    name += strlen(prefix);
    for (index = 0; index < ACE_PRIVILEGE_NONCE_LENGTH; index++) {
        unsigned value;

        if (!isxdigit((unsigned char)name[index * 2]) ||
            !isxdigit((unsigned char)name[index * 2 + 1]))
            return -1;
        if (sscanf(name + index * 2, "%2x", &value) != 1)
            return -1;
        nonce[index] = (uint8_t)value;
    }
    return 0;
}

static int send_record(int fd, const void *header, size_t header_size,
                       const void *payload, size_t payload_length)
{
    struct iovec io[2];
    struct msghdr message;
    ssize_t sent;

    memset(&message, 0, sizeof(message));
    io[0].iov_base = (void *)header;
    io[0].iov_len = header_size;
    io[1].iov_base = (void *)payload;
    io[1].iov_len = payload_length;
    message.msg_iov = io;
    message.msg_iovlen = payload && payload_length ? 2 : 1;
    do {
        sent = sendmsg(fd, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    return sent < 0 ? -1 : 0;
}

static int reply(struct fmm_state *state, uint64_t request_id,
                 int32_t status, int host_errno, const void *payload,
                 uint32_t payload_length)
{
    struct ace_privilege_response response;

    memset(&response, 0, sizeof(response));
    response.magic = ACE_PRIVILEGE_MAGIC;
    response.request_id = request_id;
    response.status = status;
    response.host_errno = host_errno;
    response.payload_length = payload_length;
    return send_record(state->fd, &response, sizeof(response), payload,
                       payload_length);
}

/*
 * Reply carrying one descriptor.
 *
 * SCM_RIGHTS has to ride along with real data, so the response header is the
 * data it accompanies.  The flag says a descriptor is present rather than
 * leaving the receiver to discover it, because "no descriptor" and "a
 * descriptor I failed to notice" must not look alike on a privileged channel.
 */
static int reply_with_fd(struct fmm_state *state, uint64_t request_id,
                         int passed_fd, int32_t worker_pid)
{
    struct ace_privilege_response response;
    struct iovec io[2];
    struct msghdr message;
    union {
        char bytes[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } control;
    struct cmsghdr *entry;
    ssize_t sent;

    memset(&response, 0, sizeof(response));
    response.magic = ACE_PRIVILEGE_MAGIC;
    response.request_id = request_id;
    response.status = ACE_PRIVILEGE_OK;
    response.flags = ACE_PRIVILEGE_FLAG_HAS_FD;
    response.payload_length = sizeof(worker_pid);

    memset(&message, 0, sizeof(message));
    io[0].iov_base = &response;
    io[0].iov_len = sizeof(response);
    io[1].iov_base = &worker_pid;
    io[1].iov_len = sizeof(worker_pid);
    message.msg_iov = io;
    message.msg_iovlen = 2;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    entry = CMSG_FIRSTHDR(&message);
    entry->cmsg_level = SOL_SOCKET;
    entry->cmsg_type = SCM_RIGHTS;
    entry->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(entry), &passed_fd, sizeof(passed_fd));
    do {
        sent = sendmsg(state->fd, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    return sent < 0 ? -1 : 0;
}

/*
 * Fork the CRM into this process's mount namespace.
 *
 * Order matters and is the whole point.  The namespace has to exist first, so
 * that the child inherits it and can see the device view; the child then
 * closes the supervisor's own channel, so that it holds no way to reach the
 * volume side.  What comes back to the broker is one end of a fresh
 * socketpair -- a second, independent conversation with a process that can
 * open files and cannot mount them.
 */
static int spawn_crm(struct fmm_state *state,
                               uint64_t request_id)
{
    int pair[2];
    pid_t child;

    /*
     * A namespace is not required.
     *
     * It was, while the device view was the only thing an CRM could
     * reach: a worker outside the namespace would have been unable to see the
     * one tree it existed for.  Now that protected host paths are the other
     * half of its job, a worker without a namespace is a perfectly good
     * worker -- it simply has no device view to offer, and says so by
     * refusing view-domain requests for want of a root to resolve them under.
     *
     * This matters for the ordinary case: a session with --root and no device
     * view wants exactly this worker, and refusing to make one would mean
     * escalation only worked for people who had also asked for a device view.
     */
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0) {
        reply(state, request_id, ACE_PRIVILEGE_HOST_ERROR, errno, NULL, 0);
        return 0;
    }
    child = fork();
    if (child < 0) {
        int failure = errno;

        close(pair[0]);
        close(pair[1]);
        reply(state, request_id, ACE_PRIVILEGE_HOST_ERROR, failure, NULL, 0);
        return 0;
    }
    if (child == 0) {
        close(pair[0]);
        /* Neither the supervisor's channel nor the volume worker's is this
           process's business.  Closing both is what makes the isolation
           structural rather than declared: an access worker that has been
           talked into misbehaving has no descriptor over which to ask for a
           mount, because it is not holding one. */
        close(state->fd);
        if (state->volume_fd >= 0)
            close(state->volume_fd);
        ace_crm_serve(pair[1], state->served_uid,
                      state->view_root[0] ? state->view_root : NULL);
        _exit(0);
    }
    close(pair[1]);
    /*
     * The child's pid travels in the payload rather than being read from the
     * socket's peer credentials, because a socketpair reports the credentials
     * of whoever created it -- this process -- and not of the child that ends
     * up using it.  Asking the kernel there would return the supervisor and
     * look like a correct answer.
     */
    if (reply_with_fd(state, request_id, pair[0], (int32_t)child) != 0) {
        close(pair[0]);
        return 1;
    }
    /* The broker owns it now. */
    close(pair[0]);
    return 0;
}

/*
 * The handshake, from the privileged side.
 *
 * Every check here is one this process makes for itself.  A root process that
 * believed what it was told about who it was talking to would be a root
 * process with a user-supplied idea of its own purpose.
 */
static int perform_handshake(struct fmm_state *state,
                             const uint8_t *expected_nonce)
{
    struct ace_privilege_hello hello;
    struct ace_privilege_hello_reply answer;
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);
    ssize_t got;
    int32_t status = ACE_PRIVILEGE_OK;

    do {
        got = recv(state->fd, &hello, sizeof(hello), 0);
    } while (got < 0 && errno == EINTR);
    if (got != (ssize_t)sizeof(hello))
        return -1;
    if (hello.magic != ACE_PRIVILEGE_MAGIC ||
        hello.version != ACE_PRIVILEGE_PROTOCOL_VERSION)
        status = ACE_PRIVILEGE_PROTOCOL_ERROR;
    else if (memcmp(hello.nonce, expected_nonce,
                    ACE_PRIVILEGE_NONCE_LENGTH) != 0)
        status = ACE_PRIVILEGE_UNAUTHORISED;
    else if (getsockopt(state->fd, SOL_SOCKET, SO_PEERCRED, &peer,
                        &peer_size) != 0 || peer_size != sizeof(peer))
        status = ACE_PRIVILEGE_PROTOCOL_ERROR;
    /* The peer must be the user this fmm was authorised for.  Not "some
       user", and not whoever the message claims to be from. */
    else if (peer.uid != state->served_uid)
        status = ACE_PRIVILEGE_UNAUTHORISED;
    /* And it must be the process it says it is.  A broker_pid that disagrees
       with the kernel's view of the connection means the message was composed
       somewhere other than the process that sent it. */
    else if (hello.broker_pid != (int32_t)peer.pid)
        status = ACE_PRIVILEGE_UNAUTHORISED;

    memset(&answer, 0, sizeof(answer));
    answer.magic = ACE_PRIVILEGE_MAGIC;
    answer.version = ACE_PRIVILEGE_PROTOCOL_VERSION;
    answer.status = status;
    if (status == ACE_PRIVILEGE_OK) {
        /* Grant no more than was asked for, and no more than exists.  A
           capability the broker did not request is one it cannot have
           reasoned about. */
        state->capabilities = hello.requested_capabilities &
                              (ACE_PRIVILEGE_CAP_VOLUME | ACE_PRIVILEGE_CAP_ACCESS);
        state->broker_pid = peer.pid;
        answer.granted_capabilities = state->capabilities;
        answer.authorisation_seconds = state->authorisation_seconds;
        if (state->authorisation_seconds)
            state->deadline_ms = now_ms() +
                                 (long long)state->authorisation_seconds * 1000;
    }
    if (send_record(state->fd, &answer, sizeof(answer), NULL, 0) != 0)
        return -1;
    return status == ACE_PRIVILEGE_OK ? 0 : -1;
}

/*
 * Hand one volume-class request to the volume worker and relay its answer.
 *
 * The record goes across unchanged and the reply comes back unchanged: the
 * supervisor is a wire between two processes that have already agreed what
 * they are saying, and a wire that reinterpreted the traffic would be a third
 * opinion about privileged work.  The one thing read out in passing is the
 * view root, because the supervisor has to know it to give an access worker
 * its subtree, and this reply is where it is stated.
 *
 * A worker that has gone is reported as a host error rather than pretended
 * away.  The broker then knows the mount side is unavailable, which is true,
 * instead of being told a mount silently did not happen.
 */
static int forward_to_volume(struct fmm_state *state,
                             const struct ace_privilege_request *request,
                             const void *payload)
{
    struct ace_privilege_response response;
    unsigned char answer[ACE_PRIVILEGE_MAX_PAYLOAD];
    struct iovec io[2];
    struct msghdr message;
    ssize_t got;

    if (state->volume_fd < 0) {
        reply(state, request->request_id, ACE_PRIVILEGE_HOST_ERROR, ESRCH,
              NULL, 0);
        return 0;
    }
    if (send_record(state->volume_fd, request, sizeof(*request), payload,
                    request->payload_length) != 0)
        goto gone;

    memset(&message, 0, sizeof(message));
    io[0].iov_base = &response;
    io[0].iov_len = sizeof(response);
    io[1].iov_base = answer;
    io[1].iov_len = sizeof(answer);
    message.msg_iov = io;
    message.msg_iovlen = 2;
    do {
        got = recvmsg(state->volume_fd, &message, 0);
    } while (got < 0 && errno == EINTR);
    if (got < (ssize_t)sizeof(response))
        goto gone;
    if (response.magic != ACE_PRIVILEGE_MAGIC ||
        response.request_id != request->request_id ||
        response.payload_length > sizeof(answer) ||
        (size_t)got < sizeof(response) + response.payload_length) {
        /* The one process this supervisor trusts is its own child, and a
           child speaking nonsense is not a child worth continuing with. */
        reply(state, request->request_id, ACE_PRIVILEGE_PROTOCOL_ERROR, 0,
              NULL, 0);
        return 1;
    }
    if (request->operation == ACE_PRIVILEGE_VOLUME_PREPARE_VIEW &&
        response.status == ACE_PRIVILEGE_OK && response.payload_length &&
        response.payload_length <= sizeof(state->view_root) &&
        answer[response.payload_length - 1] == '\0')
        memcpy(state->view_root, answer, response.payload_length);
    if (send_record(state->fd, &response, sizeof(response), answer,
                    response.payload_length) != 0)
        return 1;
    return 0;

gone:
    close(state->volume_fd);
    state->volume_fd = -1;
    reply(state, request->request_id, ACE_PRIVILEGE_HOST_ERROR, EIO, NULL, 0);
    return 0;
}

/*
 * One request.  Returns 0 to keep serving, 1 to stop.
 *
 * The order of the checks is the point.  Class membership is decided before
 * the payload is looked at -- before, specifically, any path inside it is
 * examined -- so that a request from the wrong class is refused without this
 * process ever having interpreted a byte of what it carried.
 */
static int dispatch(struct fmm_state *state,
                    const struct ace_privilege_request *request,
                    const void *payload)
{
    uint32_t class_of = ACE_PRIVILEGE_CLASS_OF(request->operation);

    if (request->magic != ACE_PRIVILEGE_MAGIC) {
        reply(state, request->request_id, ACE_PRIVILEGE_PROTOCOL_ERROR, 0,
              NULL, 0);
        return 1;
    }
    if (request->payload_length > ACE_PRIVILEGE_MAX_PAYLOAD) {
        reply(state, request->request_id, ACE_PRIVILEGE_PROTOCOL_ERROR, 0,
              NULL, 0);
        return 1;
    }
    if (class_of == ACE_PRIVILEGE_CLASS_VOLUME &&
        !(state->capabilities & ACE_PRIVILEGE_CAP_VOLUME)) {
        reply(state, request->request_id, ACE_PRIVILEGE_REFUSED, 0, NULL, 0);
        return 0;
    }
    if (class_of == ACE_PRIVILEGE_CLASS_ACCESS &&
        !(state->capabilities & ACE_PRIVILEGE_CAP_ACCESS)) {
        reply(state, request->request_id, ACE_PRIVILEGE_REFUSED, 0, NULL, 0);
        return 0;
    }

    switch (request->operation) {
    case ACE_PRIVILEGE_PING:
        reply(state, request->request_id, ACE_PRIVILEGE_OK, 0, NULL, 0);
        return 0;
    case ACE_PRIVILEGE_CAPS: {
        uint32_t granted = state->capabilities;

        reply(state, request->request_id, ACE_PRIVILEGE_OK, 0, &granted,
              sizeof(granted));
        return 0;
    }
    case ACE_PRIVILEGE_CANCEL:
        /* Nothing is ever in flight yet: this build serves one request at a
           time and answers before reading the next.  Answering OK is honest
           -- the named request is not running -- and keeps the broker's
           break path exercised from the beginning rather than bolted on
           beside the first long operation. */
        reply(state, request->request_id, ACE_PRIVILEGE_OK, 0, NULL, 0);
        return 0;
    case ACE_PRIVILEGE_DROP_PRIVILEGE:
        /* The user asked for their privilege back.  Answer first, so they
           are told it happened, then go. */
        reply(state, request->request_id, ACE_PRIVILEGE_OK, 0, NULL, 0);
        return 1;
    case ACE_PRIVILEGE_SPAWN_ACCESS:
        /* Only the supervisor makes children.  A volume worker that could
           would be a mount process with a route to a file-opening one. */
        if (state->serves_volume) {
            reply(state, request->request_id, ACE_PRIVILEGE_REFUSED, 0, NULL,
                  0);
            return 0;
        }
        return spawn_crm(state, request->request_id);
    case ACE_PRIVILEGE_SHUTDOWN:
        reply(state, request->request_id, ACE_PRIVILEGE_OK, 0, NULL, 0);
        return 1;
    default:
        break;
    }

    if (class_of == ACE_PRIVILEGE_CLASS_VOLUME) {
        char answer[ACE_PRIVILEGE_MAX_PAYLOAD];
        size_t answer_length = 0;
        int host_errno = 0;
        int status;

        /* The supervisor holds no mounts and must never learn how: the same
           code path in the volume worker serves this, one private channel
           away. */
        if (!state->serves_volume)
            return forward_to_volume(state, request, payload);
        status = ace_fmm_volume_dispatch(request, payload, state->served_uid,
                                         answer, sizeof(answer),
                                         &answer_length, &host_errno);
        reply(state, request->request_id, status, host_errno, answer,
              (uint32_t)answer_length);
        return 0;
    }
    /* Access operations are contracted but not yet implemented.  Refused
       rather than silently accepted, so a broker built ahead of its fmm
       finds out by being told. */
    if (class_of == ACE_PRIVILEGE_CLASS_ACCESS) {
        reply(state, request->request_id, ACE_PRIVILEGE_UNSUPPORTED, ENOSYS,
              NULL, 0);
        return 0;
    }
    reply(state, request->request_id, ACE_PRIVILEGE_REFUSED, 0, NULL, 0);
    return 0;
}

static void serve(struct fmm_state *state)
{
    for (;;) {
        struct ace_privilege_request request;
        unsigned char payload[ACE_PRIVILEGE_MAX_PAYLOAD];
        struct iovec io[2];
        struct msghdr message;
        struct pollfd waiting;
        ssize_t got;
        int timeout = -1;
        int ready;

        if (state->deadline_ms) {
            long long remaining = state->deadline_ms - now_ms();

            if (remaining <= 0)
                return; /* Authorisation lapsed.  Leave quietly. */
            timeout = (int)remaining;
        }
        waiting.fd = state->fd;
        waiting.events = POLLIN;
        waiting.revents = 0;
        do {
            ready = poll(&waiting, 1, timeout);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0)
            return;

        memset(&message, 0, sizeof(message));
        io[0].iov_base = &request;
        io[0].iov_len = sizeof(request);
        io[1].iov_base = payload;
        io[1].iov_len = sizeof(payload);
        message.msg_iov = io;
        message.msg_iovlen = 2;
        do {
            got = recvmsg(state->fd, &message, 0);
        } while (got < 0 && errno == EINTR);
        /*
         * Zero means the broker is gone.  That is the ordinary way this
         * process ends: a broker that crashed cannot send SHUTDOWN, so EOF
         * has to mean the same thing, and it is checked before anything else
         * because there is nothing else it could mean.
         */
        if (got <= 0)
            return;
        if ((size_t)got < sizeof(request))
            return;
        /*
         * A record that claims more payload than it brought is not a short
         * read -- these are datagrams -- so it is a sender describing itself
         * wrongly.  Checked here, before dispatch, because the supervisor
         * forwards the payload onward by that length and would otherwise be
         * handing its own uninitialised stack to a worker.
         */
        if (request.payload_length > ACE_PRIVILEGE_MAX_PAYLOAD ||
            (size_t)got < sizeof(request) + request.payload_length)
            return;
        if (dispatch(state, &request, payload))
            return;
    }
}

/*
 * Fork the volume worker into the namespace this process has already made.
 *
 * It is created once, at startup, before any request has been read: the
 * supervisor should never be in a position where a mount request arrives and
 * it has to decide whether to become capable of serving it.  The child closes
 * the broker's channel, so the only thing that can reach it is this
 * supervisor, over the one socketpair, carrying records of one class.
 */
static int start_volume_worker(struct fmm_state *state)
{
    int pair[2];
    pid_t child;

    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0)
        return -1;
    child = fork();
    if (child < 0) {
        close(pair[0]);
        close(pair[1]);
        return -1;
    }
    if (child == 0) {
        struct fmm_state worker;

        close(pair[0]);
        /* The broker is not this process's peer and never will be.  A volume
           worker holding the broker's channel would be reachable by anything
           the broker could be talked into sending, which is the arrangement
           this whole split exists to end. */
        close(state->fd);
        memset(&worker, 0, sizeof(worker));
        worker.fd = pair[1];
        worker.volume_fd = -1;
        worker.served_uid = state->served_uid;
        /* One class, fixed at birth.  Not negotiated, because there is no
           peer here to negotiate with. */
        worker.capabilities = ACE_PRIVILEGE_CAP_VOLUME;
        worker.serves_volume = 1;
        serve(&worker);
        /* Whichever way it leaves -- SHUTDOWN, or a supervisor that stopped
           existing -- the mounts this process made are its own to take
           down. */
        ace_fmm_volume_shutdown();
        close(pair[1]);
        _exit(0);
    }
    close(pair[1]);
    state->volume_fd = pair[0];
    state->volume_pid = child;
    return 0;
}

/*
 * Let the volume worker finish.
 *
 * Closing the channel is the whole instruction: EOF means the same thing to
 * it that it means here, and it needs no signal to be told so.  The wait is
 * not politeness -- the worker unmounts on its way out, and a supervisor that
 * exited first would tear the namespace down underneath that work.
 */
static void stop_volume_worker(struct fmm_state *state)
{
    if (state->volume_fd >= 0) {
        close(state->volume_fd);
        state->volume_fd = -1;
    }
    if (state->volume_pid > 0) {
        int discarded;

        while (waitpid(state->volume_pid, &discarded, 0) < 0 && errno == EINTR)
            ;
        state->volume_pid = 0;
    }
}

int main(int argc, char **argv)
{
    struct fmm_state state;
    struct sockaddr_un address;
    int host_errno = 0;
    uint8_t expected_nonce[ACE_PRIVILEGE_NONCE_LENGTH];
    const char *timeout_text;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <channel-socket>\n", argv[0]);
        return 2;
    }
    if (strlen(argv[1]) >= sizeof(address.sun_path)) {
        fprintf(stderr, "ace-fmm: channel path too long\n");
        return 2;
    }

    memset(&state, 0, sizeof(state));
    state.fd = -1;
    state.volume_fd = -1;
    state.served_uid = resolve_served_uid();

    if (nonce_from_path(argv[1], expected_nonce) != 0) {
        fprintf(stderr, "ace-fmm: channel name is not a fmm "
                        "rendezvous\n");
        return 2;
    }

    /*
     * An optional lapse.  Off unless asked for: the decision on record is
     * that --root plus one authentication lasts the session, because being
     * asked again and again to use your own computer is what ACE is trying
     * not to feel like.
     *
     * What bounds it instead is the session.  This process exits with the
     * last --root shell that was the reason for it, and the broker that
     * holds this channel now exits itself once nothing has been attached to
     * it for half an hour, so an authentication cannot outlive the day's
     * work by sitting in a broker nobody is using any more.
     */
    timeout_text = getenv("ACE_PRIVILEGE_TIMEOUT");
    if (timeout_text && *timeout_text) {
        char *end = NULL;
        unsigned long parsed = strtoul(timeout_text, &end, 10);

        if (end && *end == '\0' && parsed <= 86400u)
            state.authorisation_seconds = (uint32_t)parsed;
    }

    /* No terminal, no controlling process group, no inherited working
       directory holding a mount busy.  A root process should be reachable
       only through its channel. */
    signal(SIGINT, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    if (chdir("/") != 0)
        return 1;
    umask(022);

    state.fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (state.fd < 0) {
        perror("ace-fmm: socket");
        return 1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, argv[1]);
    if (connect(state.fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("ace-fmm: connect");
        close(state.fd);
        return 1;
    }
    if (perform_handshake(&state, expected_nonce) != 0) {
        /* Deliberately terse.  A handshake that failed did so for one of a
           few reasons, none of which a stranger should be helped to tell
           apart. */
        fprintf(stderr, "ace-fmm: refused\n");
        close(state.fd);
        return 1;
    }

    /*
     * The namespace first, then the worker that lives in it.
     *
     * Both children are forked from here and so share this namespace; that
     * sharing is what lets an access worker resolve inside a device view the
     * volume worker mounted.  Doing it the other way round -- each child
     * unsharing for itself -- would give them two views of the disks that
     * looked identical and were not.
     */
    if (ace_fmm_volume_start_namespace(&host_errno) != ACE_PRIVILEGE_OK) {
        /*
         * Not fatal, and deliberately so.  A session with no device view is
         * an ordinary session -- most are -- and the only thing lost here is
         * the ability to mount anything, which every volume request will now
         * say for itself.  Treating it as fatal would mean an ACE that could
         * not start at all on a kernel or a container that declines to hand
         * out mount namespaces.
         */
        if (geteuid() == 0)
            /* Root asked for a namespace and did not get one: that is worth
               saying.  An unprivileged fmm is a test, and was never going to
               get one, so it says nothing. */
            fprintf(stderr, "ace-fmm: no device view: %s\n",
                    strerror(host_errno));
    }
    if (start_volume_worker(&state) != 0) {
        perror("ace-fmm: volume worker");
        close(state.fd);
        return 1;
    }

    serve(&state);
    /* Whichever way we leave -- SHUTDOWN, dropped privilege, a lapsed
       authorisation, or a broker that simply stopped existing -- the children
       go with us, and the mounts go with the volume worker. */
    stop_volume_worker(&state);
    close(state.fd);
    return 0;
}
