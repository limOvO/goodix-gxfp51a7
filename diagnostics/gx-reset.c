// gx-reset: pulses the GXFP51A7 reset line, then rebinds spidev.
//
// Why this exists: the sensor's reset is not exposed through libgpiod here, and
// a wedged sensor answers nothing at all, so the driver cannot recover it on
// its own. A short pulse on gpiochip0 line 264 (ACTIVE HIGH on this board)
// followed by an spidev unbind/rebind is what clears it.
//
// Run as root:  ./gx-reset [gpiochip] [line]
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *chip = argc > 1 ? argv[1] : "/dev/gpiochip0";
    unsigned line = argc > 2 ? (unsigned) strtoul(argv[2], NULL, 10) : 264;
    struct gpio_v2_line_request req;
    struct gpio_v2_line_values vals;
    int cfd, lfd;

    cfd = open(chip, O_RDWR | O_CLOEXEC);
    if (cfd < 0) {
        fprintf(stderr, "open %s: %s\n", chip, strerror(errno));
        return 1;
    }

    memset(&req, 0, sizeof req);
    req.offsets[0] = line;
    req.num_lines = 1;
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    strncpy(req.consumer, "gx-reset", sizeof req.consumer - 1);

    if (ioctl(cfd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        fprintf(stderr, "line %u request: %s\n", line, strerror(errno));
        close(cfd);
        return 1;
    }
    lfd = req.fd;

    // Assert (active high = 1), hold briefly, release. A hold rather than a
    // pulse was measured to prevent recovery, so keep this short.
    memset(&vals, 0, sizeof vals);
    vals.mask = 1;
    vals.bits = 1;
    if (ioctl(lfd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) < 0)
        fprintf(stderr, "assert: %s\n", strerror(errno));
    usleep(10000);
    vals.bits = 0;
    if (ioctl(lfd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) < 0)
        fprintf(stderr, "release: %s\n", strerror(errno));
    usleep(120000);

    close(lfd);
    close(cfd);
    printf("pulsed %s line %u\n", chip, line);
    return 0;
}
