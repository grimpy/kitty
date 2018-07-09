//========================================================================
// GLFW 3.3 XKB - www.glfw.org
//------------------------------------------------------------------------
// Copyright (c) 2018 Kovid Goyal <kovid@kovidgoyal.net>
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would
//    be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source
//    distribution.
//
//========================================================================

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "internal.h"
#include "ibus_glfw.h"

#define debug(...) if (_glfw.hints.init.debugKeyboard) printf(__VA_ARGS__);
static const char IBUS_SERVICE[]         = "org.freedesktop.IBus";
static const char IBUS_PATH[]            = "/org/freedesktop/IBus";
static const char IBUS_INTERFACE[]       = "org.freedesktop.IBus";
static const char IBUS_INPUT_INTERFACE[] = "org.freedesktop.IBus.InputContext";
enum Capabilities {
    IBUS_CAP_PREEDIT_TEXT       = 1 << 0,
    IBUS_CAP_AUXILIARY_TEXT     = 1 << 1,
    IBUS_CAP_LOOKUP_TABLE       = 1 << 2,
    IBUS_CAP_FOCUS              = 1 << 3,
    IBUS_CAP_PROPERTY           = 1 << 4,
    IBUS_CAP_SURROUNDING_TEXT   = 1 << 5
};


static inline GLFWbool
has_env_var(const char *name, const char *val) {
    const char *q = getenv(name);
    return (q && strcmp(q, val) == 0) ? GLFW_TRUE : GLFW_FALSE;
}

static inline GLFWbool
MIN(size_t a, size_t b) {
    return a < b ? a : b;
}

// Connection handling {{{
static DBusHandlerResult
message_handler(DBusConnection *conn, DBusMessage *msg, void *user_data) {
    _GLFWIBUSData *ibus = (_GLFWIBUSData*)user_data;
    (void)ibus;
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static inline const char*
get_ibus_address_file_name(void) {
    const char *addr;
    static char ans[PATH_MAX];
    addr = getenv("IBUS_ADDRESS");
    int offset = 0;
    if (addr && addr[0]) {
        memcpy(ans, addr, MIN(strlen(addr), sizeof(ans)));
        return ans;
    }

    const char *de = getenv("DISPLAY");
    if (!de || !de[0]) de = ":0.0";
    char *display = strdup(de);
    const char *host = display;
    char *disp_num  = strrchr(display, ':');
    char *screen_num = strrchr(display, '.');

    if (!disp_num) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Could not get IBUS address file name as DISPLAY env var has no colon");
        free(display);
        return NULL;
    }
    *disp_num = 0;
    disp_num++;
    if (screen_num) *screen_num = 0;
    if (!*host) host = "unix";

    memset(ans, 0, sizeof(ans));
    const char *conf_env = getenv("XDG_CONFIG_HOME");
    if (conf_env && conf_env[0]) {
        offset = snprintf(ans, sizeof(ans), "%s", conf_env);
    } else {
        conf_env = getenv("HOME");
        if (!conf_env || !conf_env[0]) {
            _glfwInputError(GLFW_PLATFORM_ERROR, "Could not get IBUS address file name as no HOME env var is set");
            free(display);
            return NULL;
        }
        offset = snprintf(ans, sizeof(ans), "%s/.config", conf_env);
    }
    char *key = dbus_get_local_machine_id();
    snprintf(ans + offset, sizeof(ans) - offset, "/ibus/bus/%s-%s-%s", key, host, disp_num);
    dbus_free(key);
    free(display);
    return ans;
}


static inline const char*
read_ibus_address(_GLFWIBUSData *ibus) {
    static char buf[1024];
    struct stat s;
    FILE *addr_file = fopen(ibus->address_file_name, "r");
    if (!addr_file) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Failed to open IBUS address file: %s with error: %s", ibus->address_file_name, strerror(errno));
        return NULL;
    }
    int stat_result = fstat(fileno(addr_file), &s);
    GLFWbool found = GLFW_FALSE;
    while (fgets(buf, sizeof(buf), addr_file)) {
        if (strncmp(buf, "IBUS_ADDRESS=", sizeof("IBUS_ADDRESS=")-1) == 0) {
            size_t sz = strlen(buf);
            if (buf[sz-1] == '\n') buf[sz-1] = 0;
            if (buf[sz-2] == '\r') buf[sz-2] = 0;
            found = GLFW_TRUE;
            break;
        }
    }
    fclose(addr_file); addr_file = NULL;
    if (stat_result != 0) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Failed to stat IBUS address file: %s with error: %s", ibus->address_file_name, strerror(errno));
        return NULL;
    }
    ibus->address_file_mtime = s.st_mtime;
    if (found) return buf + sizeof("IBUS_ADDRESS=") - 1;
    _glfwInputError(GLFW_PLATFORM_ERROR, "Could not find IBUS_ADDRESS in %s", ibus->address_file_name);
    return NULL;
}

GLFWbool
setup_connection(_GLFWIBUSData *ibus) {
    const char *path = NULL;
    const char *client_name = "GLFW_Application";
    const char *address_file_name = get_ibus_address_file_name();
    if (!address_file_name) return GLFW_FALSE;
    free((void*)ibus->address_file_name);
    ibus->address_file_name = strdup(address_file_name);
    const char *address = read_ibus_address(ibus);
    if (!address) return GLFW_FALSE;
    free((void*)ibus->address);
    ibus->address = strdup(address);
    debug("Connecting to IBUS daemon for IME input management\n");
    ibus->conn = glfw_dbus_connect_to(ibus->address, "Failed to connect to the IBUS daemon, with error");
    if (!ibus->conn) return GLFW_FALSE;
    if (!glfw_dbus_call_method(ibus->conn, IBUS_SERVICE, IBUS_PATH, IBUS_INTERFACE, "CreateInputContext",
            DBUS_TYPE_STRING, &client_name, DBUS_TYPE_INVALID,
            DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) return GLFW_FALSE;
    free((void*)ibus->input_ctx_path);
    ibus->input_ctx_path = strdup(path);
    enum Capabilities caps = IBUS_CAP_FOCUS | IBUS_CAP_PREEDIT_TEXT;
    if (!glfw_dbus_call_void_method(ibus->conn, IBUS_SERVICE, ibus->input_ctx_path, IBUS_INPUT_INTERFACE, "SetCapabilities", DBUS_TYPE_UINT32, &caps, DBUS_TYPE_INVALID)) return GLFW_FALSE;

    dbus_connection_flush(ibus->conn);
    dbus_bus_add_match(ibus->conn, "type='signal',interface='org.freedesktop.IBus.InputContext'", NULL);

    DBusObjectPathVTable ibus_vtable = {.message_function = message_handler};
    dbus_connection_try_register_object_path(ibus->conn, ibus->input_ctx_path, &ibus_vtable, ibus, NULL);
    dbus_connection_flush(ibus->conn);
    debug("Connected to IBUS daemon for IME input management\n");
    ibus->ok = GLFW_TRUE;

    return GLFW_TRUE;
}


void
glfw_connect_to_ibus(_GLFWIBUSData *ibus, _GLFWDBUSData *dbus) {
    if (ibus->inited) return;
    ibus->inited = GLFW_TRUE;
    if (!has_env_var("XMODIFIERS", "@im=ibus") && !has_env_var("GTK_IM_MODULE", "ibus") && !has_env_var("QT_IM_MODULE", "ibus")) return;
    if (!glfw_dbus_init(dbus)) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Cannot connect to IBUS as connection to DBUS session bus failed");
    }
    setup_connection(ibus);
}

void
glfw_ibus_terminate(_GLFWIBUSData *ibus) {
    if (ibus->conn) {
        glfw_dbus_close_connection(ibus->conn);
        ibus->conn = NULL;
    }
#define F(x) if (ibus->x) { free((void*)ibus->x); ibus->x = NULL; }
    F(input_ctx_path); F(address); F(address_file_name);
#undef F

    ibus->ok = GLFW_FALSE;
}
// }}}
