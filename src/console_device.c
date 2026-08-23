#include "console_device.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct amiga_console_request_state {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    struct amiga_console_io_request *request;
    struct amiga_console_request_state *next;
    int done;
    int running;
    int aborted;
};

struct amiga_console_unit {
    struct amiga_console_device *device;
    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    pthread_cond_t input_condition;
    struct amiga_console_request_state *queue_head;
    struct amiga_console_request_state *queue_tail;
    unsigned char *input;
    size_t input_length;
    size_t input_offset;
    size_t input_capacity;
    int closing;
};

static void complete_request(struct amiga_console_request_state *state,
                             int error, size_t actual)
{
    pthread_mutex_lock(&state->lock);
    if (state->aborted) {
        state->request->error = AMIGA_IOERR_ABORTED;
        state->request->actual = 0;
    } else {
        state->request->error = error;
        state->request->actual = actual;
    }
    state->done = 1;
    pthread_cond_signal(&state->condition);
    pthread_mutex_unlock(&state->lock);
}

static int queued_input_read(struct amiga_console_unit *unit,
                             struct amiga_console_request_state *state,
                             void *data, size_t length, size_t *actual)
{
    size_t count;

    if (length == 0) {
        *actual = 0;
        return AMIGA_IOERR_OK;
    }
    pthread_mutex_lock(&unit->lock);
    while (unit->input_length == unit->input_offset && !unit->closing) {
        int aborted;

        pthread_mutex_lock(&state->lock);
        aborted = state->aborted;
        pthread_mutex_unlock(&state->lock);
        if (aborted) {
            pthread_mutex_unlock(&unit->lock);
            return AMIGA_IOERR_ABORTED;
        }
        pthread_cond_wait(&unit->input_condition, &unit->lock);
    }
    if (unit->closing) {
        pthread_mutex_unlock(&unit->lock);
        return AMIGA_IOERR_ABORTED;
    }
    count = unit->input_length - unit->input_offset;
    if (count > length)
        count = length;
    memcpy(data, unit->input + unit->input_offset, count);
    unit->input_offset += count;
    if (unit->input_offset == unit->input_length) {
        unit->input_offset = 0;
        unit->input_length = 0;
    }
    pthread_mutex_unlock(&unit->lock);
    *actual = count;
    return AMIGA_IOERR_OK;
}

static void *console_worker(void *context)
{
    struct amiga_console_unit *unit = context;

    for (;;) {
        struct amiga_console_request_state *state;
        struct amiga_console_io_request *request;
        size_t actual = 0;
        int error;

        pthread_mutex_lock(&unit->lock);
        while (!unit->queue_head && !unit->closing)
            pthread_cond_wait(&unit->condition, &unit->lock);
        if (!unit->queue_head && unit->closing) {
            pthread_mutex_unlock(&unit->lock);
            break;
        }
        state = unit->queue_head;
        unit->queue_head = state->next;
        if (!unit->queue_head)
            unit->queue_tail = NULL;
        state->next = NULL;
        pthread_mutex_lock(&state->lock);
        state->running = 1;
        pthread_mutex_unlock(&state->lock);
        pthread_mutex_unlock(&unit->lock);

        request = state->request;
        if (request->command == AMIGA_CMD_READ) {
            if (unit->device->read)
                error = unit->device->read(unit->device->context,
                                           request->data, request->length,
                                           &actual);
            else
                error = queued_input_read(unit, state, request->data,
                                          request->length, &actual);
        } else if (request->command == AMIGA_CMD_WRITE) {
            if (!unit->device->write)
                error = AMIGA_IOERR_NOCMD;
            else
                error = unit->device->write(unit->device->context,
                                            request->data, request->length,
                                            &actual);
        } else {
            error = AMIGA_IOERR_NOCMD;
        }
        complete_request(state, error, actual);
    }
    return NULL;
}

void amiga_console_InitIO(struct amiga_console_io_request *request)
{
    if (request) {
        request->actual = 0;
        request->error = AMIGA_IOERR_OK;
        request->private_state = NULL;
    }
}

int amiga_console_OpenDevice(struct amiga_console_device *device,
                             struct amiga_console_unit **unit_out)
{
    struct amiga_console_unit *unit;
    int lock_initialized = 0;
    int condition_initialized = 0;
    int input_condition_initialized = 0;

    if (!device || !unit_out)
        return AMIGA_IOERR_BADADDRESS;
    if (!device->read && !device->write)
        return AMIGA_IOERR_BADADDRESS;
    unit = calloc(1, sizeof(*unit));
    if (!unit)
        return AMIGA_IOERR_UNITBUSY;
    unit->device = device;
    if (pthread_mutex_init(&unit->lock, NULL) == 0)
        lock_initialized = 1;
    if (lock_initialized && pthread_cond_init(&unit->condition, NULL) == 0)
        condition_initialized = 1;
    if (condition_initialized &&
        pthread_cond_init(&unit->input_condition, NULL) == 0)
        input_condition_initialized = 1;
    if (!input_condition_initialized ||
        pthread_create(&unit->worker, NULL, console_worker, unit) != 0) {
        if (input_condition_initialized)
            pthread_cond_destroy(&unit->input_condition);
        if (condition_initialized)
            pthread_cond_destroy(&unit->condition);
        if (lock_initialized)
            pthread_mutex_destroy(&unit->lock);
        free(unit);
        return AMIGA_IOERR_UNITBUSY;
    }
    *unit_out = unit;
    return AMIGA_IOERR_OK;
}

void amiga_console_CloseDevice(struct amiga_console_unit *unit)
{
    struct amiga_console_request_state *state;

    if (!unit)
        return;
    pthread_mutex_lock(&unit->lock);
    unit->closing = 1;
    state = unit->queue_head;
    unit->queue_head = NULL;
    unit->queue_tail = NULL;
    pthread_cond_broadcast(&unit->condition);
    pthread_cond_broadcast(&unit->input_condition);
    pthread_mutex_unlock(&unit->lock);

    while (state) {
        struct amiga_console_request_state *next = state->next;

        pthread_mutex_lock(&state->lock);
        state->aborted = 1;
        state->done = 1;
        state->request->actual = 0;
        state->request->error = AMIGA_IOERR_ABORTED;
        pthread_cond_signal(&state->condition);
        pthread_mutex_unlock(&state->lock);
        state = next;
    }
    pthread_join(unit->worker, NULL);
    free(unit->input);
    pthread_cond_destroy(&unit->input_condition);
    pthread_cond_destroy(&unit->condition);
    pthread_mutex_destroy(&unit->lock);
    free(unit);
}

int amiga_console_SendIO(struct amiga_console_unit *unit,
                         struct amiga_console_io_request *request)
{
    struct amiga_console_request_state *state;

    if (!unit || !request)
        return AMIGA_IOERR_BADADDRESS;
    if (!request->data && request->length != 0)
        return AMIGA_IOERR_BADADDRESS;
    if (request->private_state)
        return AMIGA_IOERR_UNITBUSY;
    state = calloc(1, sizeof(*state));
    if (!state)
        return AMIGA_IOERR_UNITBUSY;
    if (pthread_mutex_init(&state->lock, NULL) != 0) {
        free(state);
        return AMIGA_IOERR_UNITBUSY;
    }
    if (pthread_cond_init(&state->condition, NULL) != 0) {
        pthread_mutex_destroy(&state->lock);
        free(state);
        return AMIGA_IOERR_UNITBUSY;
    }
    state->request = request;
    amiga_console_InitIO(request);
    request->private_state = state;
    pthread_mutex_lock(&unit->lock);
    if (unit->closing) {
        pthread_mutex_unlock(&unit->lock);
        request->private_state = NULL;
        pthread_cond_destroy(&state->condition);
        pthread_mutex_destroy(&state->lock);
        free(state);
        request->error = AMIGA_IOERR_ABORTED;
        return request->error;
    }
    if (unit->queue_tail)
        unit->queue_tail->next = state;
    else
        unit->queue_head = state;
    unit->queue_tail = state;
    pthread_cond_signal(&unit->condition);
    pthread_mutex_unlock(&unit->lock);
    return AMIGA_IOERR_OK;
}

int amiga_console_WaitIO(struct amiga_console_io_request *request)
{
    struct amiga_console_request_state *state;
    int error;

    if (!request || !request->private_state)
        return AMIGA_IOERR_BADADDRESS;
    state = request->private_state;
    pthread_mutex_lock(&state->lock);
    while (!state->done)
        pthread_cond_wait(&state->condition, &state->lock);
    error = request->error;
    pthread_mutex_unlock(&state->lock);
    pthread_cond_destroy(&state->condition);
    pthread_mutex_destroy(&state->lock);
    free(state);
    request->private_state = NULL;
    return error;
}

int amiga_console_AbortIO(struct amiga_console_unit *unit,
                          struct amiga_console_io_request *request)
{
    struct amiga_console_request_state *state;
    struct amiga_console_request_state **cursor;
    int was_running;

    if (!unit || !request || !request->private_state)
        return AMIGA_IOERR_BADADDRESS;
    state = request->private_state;
    pthread_mutex_lock(&unit->lock);
    pthread_mutex_lock(&state->lock);
    if (state->done) {
        pthread_mutex_unlock(&state->lock);
        pthread_mutex_unlock(&unit->lock);
        return AMIGA_IOERR_OK;
    }
    state->aborted = 1;
    was_running = state->running;
    if (request->command == AMIGA_CMD_READ)
        pthread_cond_broadcast(&unit->input_condition);
    if (!was_running) {
        cursor = &unit->queue_head;
        while (*cursor && *cursor != state)
            cursor = &(*cursor)->next;
        if (*cursor == state) {
            *cursor = state->next;
            if (unit->queue_tail == state) {
                unit->queue_tail = unit->queue_head;
                while (unit->queue_tail && unit->queue_tail->next)
                    unit->queue_tail = unit->queue_tail->next;
            }
            state->done = 1;
            request->error = AMIGA_IOERR_ABORTED;
            request->actual = 0;
            pthread_cond_signal(&state->condition);
        }
    }
    pthread_mutex_unlock(&state->lock);
    pthread_mutex_unlock(&unit->lock);
    return AMIGA_IOERR_OK;
}

int amiga_console_DoIO(struct amiga_console_unit *unit,
                       struct amiga_console_io_request *request)
{
    int error;

    if (!unit || !request)
        return AMIGA_IOERR_BADADDRESS;
    error = amiga_console_SendIO(unit, request);
    if (error != AMIGA_IOERR_OK)
        return error;
    return amiga_console_WaitIO(request);
}

int amiga_console_FeedInput(struct amiga_console_unit *unit, const void *data,
                            size_t length)
{
    size_t needed;
    size_t capacity;
    unsigned char *new_input;

    if (!unit || (!data && length != 0))
        return AMIGA_IOERR_BADADDRESS;
    if (unit->device->read)
        return AMIGA_IOERR_NOCMD;
    pthread_mutex_lock(&unit->lock);
    if (unit->closing) {
        pthread_mutex_unlock(&unit->lock);
        return AMIGA_IOERR_ABORTED;
    }
    if (unit->input_offset != 0) {
        if (unit->input_length != unit->input_offset)
            memmove(unit->input, unit->input + unit->input_offset,
                    unit->input_length - unit->input_offset);
        unit->input_length -= unit->input_offset;
        unit->input_offset = 0;
    }
    needed = unit->input_length + length;
    if (needed > unit->input_capacity) {
        capacity = unit->input_capacity ? unit->input_capacity : 256;
        while (capacity < needed)
            capacity *= 2;
        new_input = realloc(unit->input, capacity);
        if (!new_input) {
            pthread_mutex_unlock(&unit->lock);
            return AMIGA_IOERR_UNITBUSY;
        }
        unit->input = new_input;
        unit->input_capacity = capacity;
    }
    /* A zero-length write with a NULL buffer is a legitimate no-op for the
       caller, and memcpy() is not allowed a NULL source even for none. */
    if (length)
        memcpy(unit->input + unit->input_length, data, length);
    unit->input_length += length;
    pthread_cond_signal(&unit->input_condition);
    pthread_mutex_unlock(&unit->lock);
    return AMIGA_IOERR_OK;
}

int amiga_con_Open(struct amiga_console_unit *unit, struct amiga_con_file *file)
{
    if (!unit || !file)
        return AMIGA_IOERR_BADADDRESS;
    file->unit = unit;
    return AMIGA_IOERR_OK;
}

void amiga_con_Close(struct amiga_con_file *file)
{
    if (file)
        file->unit = NULL;
}

int amiga_con_Read(struct amiga_con_file *file, void *data, size_t length,
                   size_t *actual)
{
    struct amiga_console_io_request request = {
        .command = AMIGA_CMD_READ,
        .data = data,
        .length = length,
    };
    int error = amiga_console_DoIO(file ? file->unit : NULL, &request);

    if (actual)
        *actual = request.actual;
    return error;
}

int amiga_con_Write(struct amiga_con_file *file, const void *data, size_t length,
                    size_t *actual)
{
    struct amiga_console_io_request request = {
        .command = AMIGA_CMD_WRITE,
        .data = (void *)data,
        .length = length,
    };
    int error = amiga_console_DoIO(file ? file->unit : NULL, &request);

    if (actual)
        *actual = request.actual;
    return error;
}
