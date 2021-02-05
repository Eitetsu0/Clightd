#include <commons.h>
#include <sys/inotify.h>
#include <module/map.h>
#include <linux/limits.h>
#include <math.h>
#include <stddef.h>
#include <polkit.h>

#define BUF_LEN (sizeof(struct inotify_event) + NAME_MAX + 1)

typedef struct {
    bool in_use;                // Whether the client has already been requested by someone
    bool is_idle;               // Whether the client is in idle state
    bool running;               // Whether "Start" method has been called on Client
    unsigned int timeout;
    unsigned int id;            // Client's id
    int fd;                     // Client's timer fd
    char *sender;               // BusName who requested this client
    char path[PATH_MAX + 1];    // Client's object path
    sd_bus_slot *slot;          // vtable's slot
} idle_client_t;

static void dtor_client(void *client);
static map_ret_code leave_idle(void *userdata, const char *key, void *client);
static map_ret_code find_free_client(void *out, const char *key, void *client);
static idle_client_t *find_available_client(void);
static void destroy_client(idle_client_t *c);
static int method_get_client(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int method_rm_client(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int method_start_client(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int method_stop_client(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int set_timeout(sd_bus *b, const char *path, const char *interface, const char *property, 
                     sd_bus_message *value, void *userdata, sd_bus_error *error);

static map_t *clients;
static int inot_fd;
static int inot_wd;
static int idler;           // how many idle clients do we have?
static int running_clients; // how many running clients do we have?
static time_t last_input;   // last /dev/input event time
static const char object_path[] = "/org/clightd/clightd/Idle";
static const char bus_interface[] = "org.clightd.clightd.Idle";
static const char clients_interface[] = "org.clightd.clightd.Idle.Client";
static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetClient", NULL, "o", method_get_client, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DestroyClient", "o", NULL, method_rm_client, SD_BUS_VTABLE_UNPRIVILEGED | SD_BUS_VTABLE_METHOD_NO_REPLY),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable vtable_clients[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Start", NULL, NULL, method_start_client, SD_BUS_VTABLE_UNPRIVILEGED | SD_BUS_VTABLE_METHOD_NO_REPLY),
    SD_BUS_METHOD("Stop", NULL, NULL, method_stop_client, SD_BUS_VTABLE_UNPRIVILEGED | SD_BUS_VTABLE_METHOD_NO_REPLY),
    SD_BUS_WRITABLE_PROPERTY("Timeout", "u", NULL, set_timeout, offsetof(idle_client_t, timeout), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("Idle", "b", 0),
    SD_BUS_VTABLE_END
};

MODULE("IDLE");

static void module_pre_start(void) {
    
}

static bool check(void) {
    return true;
}

static bool evaluate(void) {
    return true;
}

static void init(void) {
    clients = map_new(true, dtor_client);
    int r = sd_bus_add_object_vtable(bus,
                                     NULL,
                                     object_path,
                                     bus_interface,
                                     vtable,
                                     NULL);
    if (r < 0) {
        m_log("Failed to issue method call: %s\n", strerror(-r));
    }
    inot_fd = inotify_init();
    m_register_fd(inot_fd, true, NULL);
}

static void receive(const msg_t *msg, const void *userdata) {
    if (!msg->is_pubsub) {
        /* Event on /dev/input! */
        if (msg->fd_msg->fd == inot_fd) {
            char buffer[BUF_LEN];
            int length = read(msg->fd_msg->fd, buffer, BUF_LEN);
            if (length > 0) {
                /* Update our last input timer */
                last_input = time(NULL);
                /* If there is at least 1 idle client, leave idle! */
                if (idler) {
                    m_log("Leaving idle state.\n");
                    map_iterate(clients, leave_idle, NULL);
                }
            }
        } else {
            idle_client_t *c = (idle_client_t *)msg->fd_msg->userptr;
            if (c) {
                uint64_t t;
                read(msg->fd_msg->fd, &t, sizeof(uint64_t));
            
                const time_t idle_t = time(NULL) - last_input;
                c->is_idle = idle_t >= c->timeout;
                struct itimerspec timerValue = {{0}};
                if (c->is_idle) {
                    idler++;
                    sd_bus_emit_signal(bus, c->path, clients_interface, "Idle", "b", c->is_idle);
                } else {
                    timerValue.it_value.tv_sec = c->timeout - idle_t;
                }
                timerfd_settime(msg->fd_msg->fd, 0, &timerValue, NULL);
                m_log("Client %d -> Idle: %d\n", c->id, c->is_idle);
            }
        }
    }
}

static void destroy(void) {
    if (running_clients > 0) {
        inotify_rm_watch(inot_fd, inot_wd);
    }
    map_free(clients);
}

static map_ret_code leave_idle(void *userdata, const char *key, void *client) {
    idle_client_t *c = (idle_client_t *)client;
    if (c->is_idle) {
        c->is_idle = false;
        sd_bus_emit_signal(bus, c->path, clients_interface, "Idle", "b", c->is_idle);
        idler--;
        struct itimerspec timerValue = {{0}};
        timerValue.it_value.tv_sec = c->timeout;
        timerfd_settime(c->fd, 0, &timerValue, NULL);
    }
    return MAP_OK;
}

static void dtor_client(void *client) {
    idle_client_t *c = (idle_client_t *)client;
    if (c->in_use) {
        destroy_client(c);
    }
    free(c);
}

static map_ret_code find_free_client(void *out, const char *key, void *client) {
    idle_client_t *c = (idle_client_t *)client;
    idle_client_t **o = (idle_client_t **)out;
    
    if (!c->in_use) {
        *o = c;
        m_log("Returning unused client %u\n", c->id);
        return MAP_FULL; // break iteration
    }
    return MAP_OK;
}

static idle_client_t *find_available_client(void) {
    idle_client_t *c = NULL;
    if (map_iterate(clients, find_free_client, &c) != MAP_FULL) {
        /* no unused clients found. */
        c = calloc(1, sizeof(idle_client_t));
        if (c) {
            c->id = map_length(clients);
            m_log("Creating client %u\n", c->id);
        }
    }
    return c;
}

static void destroy_client(idle_client_t *c) {
    m_deregister_fd(c->fd);
    free(c->sender);
    c->slot = sd_bus_slot_unref(c->slot);
    m_log("Freeing client %u\n", c->id);
}

static idle_client_t *validate_client(const char *path, sd_bus_message *m, sd_bus_error *ret_error) {
    idle_client_t *c = map_get(clients, path);
    if (c && c->in_use && !strcmp(c->sender, sd_bus_message_get_sender(m))) {
        return c;
    }
    m_log("Failed to validate client.\n");
    sd_bus_error_set_errno(ret_error, EPERM);
    return NULL;
}

static int method_get_client(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    ASSERT_AUTH();

    idle_client_t *c = find_available_client();
    if (c) {
        c->in_use = true;
        c->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        m_register_fd(c->fd, true, c);
        c->sender = strdup(sd_bus_message_get_sender(m));
        snprintf(c->path, sizeof(c->path) - 1, "%s/Client%u", object_path, c->id);

        map_put(clients, c->path, c);
        
        sd_bus_add_object_vtable(bus,
                                &c->slot,
                                c->path,
                                clients_interface,
                                vtable_clients,
                                c);
        return sd_bus_reply_method_return(m, "o", c->path);
    }
    sd_bus_error_set_errno(ret_error, ENOMEM);
    return -ENOMEM;
}

static int method_rm_client(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    ASSERT_AUTH();
    
    /* Read the parameters */
    const char *obj_path = NULL;
    int r = sd_bus_message_read(m, "o", &obj_path);
    if (r < 0) {
        m_log("Failed to parse parameters: %s\n", strerror(-r));
        return r;
    }
    
    idle_client_t *c = validate_client(obj_path, m, ret_error);
    if (c) {
        /* You can only remove stopped clients */ 
        if (!c->running) {
            destroy_client(c);
            int id = c->id;
            memset(c, 0, sizeof(idle_client_t));
            c->id = id; // avoid zeroing client id
            return sd_bus_reply_method_return(m, NULL);
        }
        sd_bus_error_set_errno(ret_error, EINVAL);
    }
    return -sd_bus_error_get_errno(ret_error);
}

static int method_start_client(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {    
    idle_client_t *c = validate_client(sd_bus_message_get_path(m), m, ret_error);
    if (c) {
        /* You can only start not-started clients, that must have Timeout setted */
        if (c->timeout > 0 && !c->running) {
            struct itimerspec timerValue = {{0}};
            timerValue.it_value.tv_sec = c->timeout;
            timerfd_settime(c->fd, 0, &timerValue, NULL);
            c->running = true;
            if (++running_clients == 1) {
                /* Ok, start listening on /dev/input events as first client was started */
                m_log("Adding inotify watch as first client was started.\n");
                inot_wd = inotify_add_watch(inot_fd, "/dev/input/", IN_ACCESS);
            }
            m_log("Starting Client %u\n", c->id);
            return sd_bus_reply_method_return(m, NULL);
        }
        sd_bus_error_set_errno(ret_error, EINVAL);
    }
    return -sd_bus_error_get_errno(ret_error);
}

static int method_stop_client(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {    
    idle_client_t *c = validate_client(sd_bus_message_get_path(m), m, ret_error);
    if (c) {
        /* You can only stop running clients */
        if (c->running) {
            leave_idle(NULL, NULL, c);
            /* Do not reset timerfd is client is in idle state */
            struct itimerspec timerValue = {{0}};
            timerfd_settime(c->fd, 0, &timerValue, NULL);
            
            if (--running_clients == 0) {
                /* this is the only running client; remove watch on /dev/input */
                m_log("Removing inotify watch as only client using it was stopped.\n");
                inotify_rm_watch(inot_fd, inot_wd);
            }
            
            /* Reset client state */
            c->running = false;
            m_log("Stopping Client %u\n", c->id);
            return sd_bus_reply_method_return(m, NULL);
        }
        sd_bus_error_set_errno(ret_error, EINVAL);
    }
    return -sd_bus_error_get_errno(ret_error);
}

static int set_timeout(sd_bus *b, const char *path, const char *interface, const char *property, 
                       sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {    
    idle_client_t *c = validate_client(path, m, ret_error);
    if (!c) {
        return -sd_bus_error_get_errno(ret_error);
    }
    
    int old_timer  = *(int *)userdata;
    int r = sd_bus_message_read(m, "u", userdata);
    if (r < 0) {
        m_log("Failed to set timeout.\n");
        return r;
    }

    if (c->running && !c->is_idle) {
        struct itimerspec timerValue = {{0}};

        int new_timer = *(int *)userdata;
        timerfd_gettime(c->fd, &timerValue);
        int old_elapsed = old_timer - timerValue.it_value.tv_sec;
        int new_timeout = new_timer - old_elapsed;
        if (new_timeout <= 0) {
            timerValue.it_value.tv_nsec = 1;
            timerValue.it_value.tv_sec = 0;
            m_log("Starting now.\n");
        } else {
            timerValue.it_value.tv_sec = new_timeout;
            m_log("Next timer: %d\n", new_timeout);
        }
        r = timerfd_settime(c->fd, 0, &timerValue, NULL);
    }
    return r;
}
