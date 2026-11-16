/* tap: injects one finger tap at (x, y) into the touch panel's event node, a
 * drag, or a power button press, so the UI can be driven over SSH.
 * Usage: tap x y [device]
 *        tap drag x0 y0 x1 y1 [steps] [device]
 *        tap power [device]
 *        tap which        prints the devices it would use
 * Without a device it finds them the way the apps do (eink.h input_open):
 * the touch panel is the node with multi-touch positions, the power button
 * the first node named like "onkey" or "power". That is event1 and event0 on
 * the Paperwhite 3, and event2 and event0 on the Paperwhite 4. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

static void emit(int fd, int type, int code, int value) {
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.code = code; e.value = value;
    write(fd, &e, sizeof e);
}

/* The first /dev/input/eventN that is the touch panel (want_touch) or the
 * power button; "" if there is none. */
static const char *find_dev(int want_touch) {
    static char path[32];
    for (int n = 0; n < 8; n++) {
        char name[64] = "";
        snprintf(path, sizeof path, "/dev/input/event%d", n);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        struct input_absinfo ax;
        int touch = ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) == 0 && ax.maximum > 0;
        close(fd);
        if (want_touch ? touch : !touch && (strstr(name, "onkey") || strstr(name, "power"))) return path;
    }
    return "";
}

static int open_dev(const char *given, int touch) {
    const char *p = given ? given : find_dev(touch);
    int fd = p[0] ? open(p, O_WRONLY) : -1;
    if (fd < 0) fprintf(stderr, "tap: no %s device%s%s\n", touch ? "touch" : "power button", p[0] ? " at " : "", p);
    return fd;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "which") == 0) {
        printf("touch %s\n", find_dev(1));
        printf("power %s\n", find_dev(0));
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "power") == 0) {
        int fd = open_dev(argc > 2 ? argv[2] : 0, 0);
        if (fd < 0) return 1;
        emit(fd, EV_KEY, KEY_POWER, 1);
        emit(fd, EV_SYN, SYN_REPORT, 0);
        usleep(80000);
        emit(fd, EV_KEY, KEY_POWER, 0);
        emit(fd, EV_SYN, SYN_REPORT, 0);
        return 0;
    }
    if (argc >= 6 && strcmp(argv[1], "drag") == 0) {
        int x0 = atoi(argv[2]), y0 = atoi(argv[3]), x1 = atoi(argv[4]), y1 = atoi(argv[5]);
        int steps = argc > 6 ? atoi(argv[6]) : 30;
        if (steps < 1) steps = 1;
        int fd = open_dev(argc > 7 ? argv[7] : 0, 1);
        if (fd < 0) return 1;
        emit(fd, EV_ABS, ABS_MT_TRACKING_ID, 78);
        for (int i = 0; i <= steps; i++) {
            emit(fd, EV_ABS, ABS_MT_POSITION_X, x0 + (x1 - x0) * i / steps);
            emit(fd, EV_ABS, ABS_MT_POSITION_Y, y0 + (y1 - y0) * i / steps);
            emit(fd, EV_SYN, SYN_REPORT, 0);
            usleep(12000);
        }
        emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
        emit(fd, EV_SYN, SYN_REPORT, 0);
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: tap x y [device] | tap drag x0 y0 x1 y1 [steps] [device] | tap power [device] | tap which\n");
        return 2;
    }
    int fd = open_dev(argc > 3 ? argv[3] : 0, 1);
    if (fd < 0) return 1;
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, 77);
    emit(fd, EV_ABS, ABS_MT_POSITION_X, atoi(argv[1]));
    emit(fd, EV_ABS, ABS_MT_POSITION_Y, atoi(argv[2]));
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(80000);
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    return 0;
}
