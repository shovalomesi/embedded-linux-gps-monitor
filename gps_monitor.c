/*
 * Project: GPS UART Monitor
 * Author: Shoval Omesi
 * Platform: BeagleBone Green (Embedded Linux)
 * Language: C
 *
 * Description:
 * Receives NMEA GPS sentences through UART1, validates
 * message checksums, parses GGA messages, and displays
 * GPS positioning information in real time.
 *
 * Features:
 * - UART communication using POSIX termios
 * - NMEA GGA message parsing
 * - Checksum validation
 * - GPS coordinate conversion
 * - Communication timeout detection using poll()
 * - Graceful shutdown using SIGINT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <poll.h>
#include <time.h>

/* UART device and NMEA buffer configuration */
#ifndef GPS_DEVICE
#define GPS_DEVICE "/dev/ttyS1"
#endif
#define LINE_SIZE 256

/* Communication timing configuration */
#define POLL_TIMEOUT_MS 500
#define GPS_TIMEOUT_SEC 5

volatile sig_atomic_t running = 1;

void handle_sigint(int signum)
{
    (void)signum;
    running = 0;
}

/* Validate NMEA checksum */
int validate_checksum(const char *line)
{
    if (line == NULL || line[0] != '$')
        return 0;

    const char *star = strchr(line, '*');

    if (star == NULL)
        return 0;

    /* Require exactly two hexadecimal digits */
    if (strlen(star + 1) != 2)
        return 0;

    unsigned int received;

    if (!isxdigit((unsigned char)star[1]) ||
        !isxdigit((unsigned char)star[2]))
        return 0;

    if (sscanf(star + 1, "%2x", &received) != 1)
        return 0;

    unsigned char checksum = 0;

    /* XOR all characters between '$' and '*' */
    for (const char *p = line + 1; p < star; p++) {
        checksum ^= (unsigned char)*p;
    }

    return checksum == received;
}

/*
 * Convert NMEA coordinates to decimal degrees.
 * Return 1 on success or 0 if the coordinate is invalid.
 */
int convert_coordinate(const char *value,
                       int max_degrees,
                       double *coordinate)
{
    char *end;

    if (value == NULL || *value == '\0' ||
        coordinate == NULL)
        return 0;

    /* Convert the numeric string and check for errors */
    errno = 0;
    double raw = strtod(value, &end);

    if (errno != 0 || end == value || *end != '\0')
        return 0;

    /* Reject invalid values before integer conversion */
    if (!(raw >= 0.0 && raw <= max_degrees * 100.0))
        return 0;

    int degrees = (int)(raw / 100);
    double minutes = raw - degrees * 100;

    /* Validate the coordinate range */
    if (minutes < 0.0 || minutes >= 60.0)
        return 0;

    *coordinate = degrees + minutes / 60.0;

    return 1;
}

/* Parse and display a GGA message */
void process_gga(char *line)
{
    char *fields[20] = {0};
    int count = 0;

    /* Split the message at each comma, keeping empty fields */
    fields[count++] = line;

    for (char *p = line; *p && count < 20; p++) {
        if (*p == ',') {
            *p = '\0';
            fields[count++] = p + 1;
        }
    }

    if (count < 10 || strlen(fields[1]) < 6)
        return;

    int hour, minute, second;

    /* Validate UTC time format and range */
    for (int i = 0; i < 6; i++) {
        if (!isdigit((unsigned char)fields[1][i]))
            return;
    }

    if (sscanf(fields[1], "%2d%2d%2d",
               &hour, &minute, &second) != 3)
        return;

    if (hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 ||
        second < 0 || second > 60)
        return;

    /* Validate GPS fix quality */
    if (strlen(fields[6]) != 1 ||
        fields[6][0] < '0' || fields[6][0] > '8')
        return;

    int fix = fields[6][0] - '0';
    
    double latitude = 0.0;
    double longitude = 0.0;

    /* Validate and convert coordinates when a GPS fix is available */
    if (fix > 0) {
        if (!convert_coordinate(fields[2], 90, &latitude) ||
            !convert_coordinate(fields[4], 180, &longitude) ||
            (strcmp(fields[3], "N") != 0 &&
             strcmp(fields[3], "S") != 0) ||
            (strcmp(fields[5], "E") != 0 &&
             strcmp(fields[5], "W") != 0)) {

            printf("\033[2J\033[H");
            printf("GPS ERROR: Invalid or missing coordinates\n");
            fflush(stdout);
            return;
        }
    }

    printf("\033[2J\033[H");
    
    printf("\n========================================\n");
    printf("             GPS LIVE DATA              \n");
    printf("========================================\n");
    printf(" UTC Time   : %02d:%02d:%02d\n",
           hour, minute, second);

    if (fix == 0) {
        printf(" GPS Status : NO FIX\n");
        printf(" Waiting for satellite positioning...\n");
    } else {
        if (fields[3][0] == 'S')
            latitude = -latitude;

        if (fields[5][0] == 'W')
            longitude = -longitude;

        printf(" Latitude   : %.6f degrees\n", latitude);
        printf(" Longitude  : %.6f degrees\n", longitude);
        printf(" Satellites : %s\n", fields[7]);
        printf(" HDOP       : %s\n", fields[8]);
        printf(" Altitude   : %s meters\n", fields[9]);
        printf(" GPS Status : FIX OK\n");
    }

    printf("========================================\n");
    fflush(stdout);
}


int main(void)
{
    /* Handle Ctrl+C (SIGINT) */
    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    /* Open UART device */
    int fd = open(GPS_DEVICE, O_RDONLY | O_NOCTTY);

    if (fd == -1) {
        perror("Cannot open GPS device");
        return 1;
    }

    struct termios tty;

    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr failed");
        close(fd);
        return 1;
    }

    /* Configure UART: 9600 baud, 8N1, raw mode */
    cfmakeraw(&tty);
    cfsetispeed(&tty, B9600);
    cfsetospeed(&tty, B9600);

    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);

    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr failed");
        close(fd);
        return 1;
    }

    printf("GPS Monitor started.\n");
    printf("Reading from %s\n", GPS_DEVICE);
    printf("Waiting for GGA messages...\n");
    printf("Press Ctrl+C to stop.\n");
    fflush(stdout);

    char line[LINE_SIZE];
    int position = 0;
    char ch;

    /* Track the last valid GGA message */
    struct timespec last_gga_time;

    if (clock_gettime(CLOCK_MONOTONIC, &last_gga_time) == -1) {
        perror("clock_gettime failed");
        close(fd);
        return 1;
    }
    int warning_shown = 0;
    int exit_status = 0;

    struct pollfd gps_poll = {
        .fd = fd,
        .events = POLLIN,
        .revents = 0
    };

    while (running) {

        /* Wait up to 500 milliseconds for UART data */
        int result = poll(&gps_poll, 1, POLL_TIMEOUT_MS);

        if (result < 0) {
            if (errno == EINTR)
                continue;

            perror("poll failed");
            exit_status = 1;
            break;
        }

        if (!running)
            break;

        if (result > 0) {

            if (gps_poll.revents & (POLLERR | POLLNVAL)) {
                fprintf(stderr, "UART polling error\n");
                exit_status = 1;
                break;
            }

            if (gps_poll.revents & POLLHUP) {
                fprintf(stderr, "UART disconnected\n");
                exit_status = 1;
                break;
            }

            if (gps_poll.revents & POLLIN) {

                ssize_t bytes = read(fd, &ch, 1);

                if (bytes < 0) {
                    if (errno == EINTR)
                        continue;

                    perror("UART read failed");
                    exit_status = 1;
                    break;
                }

                if (bytes == 0)
                    continue;

                if (ch == '\n') {
                    line[position] = '\0';

                    if (strncmp(line, "$GNGGA,", 7) == 0 ||
                        strncmp(line, "$GPGGA,", 7) == 0) {

                        if (validate_checksum(line)) {
                            if (clock_gettime(CLOCK_MONOTONIC, &last_gga_time) == -1) {
                                perror("clock_gettime failed");
                                exit_status = 1;
                                break;
                            }
                            warning_shown = 0;
                            process_gga(line);
                        }
                    }

                    position = 0;
                }
                else if (ch != '\r') {
                    if (position < LINE_SIZE - 1) {
                        line[position++] = ch;
                    } else {
                        position = 0;
                    }
                }
            }
        }

        /* Measure elapsed time using a monotonic clock */
        struct timespec now;

        if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
            perror("clock_gettime failed");
            exit_status = 1;
            break;
        }

        double elapsed =
            difftime(now.tv_sec, last_gga_time.tv_sec) +
            (now.tv_nsec - last_gga_time.tv_nsec) / 1000000000.0;

        /* Warn if no valid GGA message arrives within the timeout */
        if (!warning_shown && elapsed >= GPS_TIMEOUT_SEC) {

            printf("\033[2J\033[H");
            printf("\n========================================\n");
            printf("             GPS LIVE DATA              \n");
            printf("========================================\n");
            printf(" GPS WARNING: No new GPS data received!\n");
            printf(" Waiting for GPS connection...\n");
            printf("========================================\n");

            fflush(stdout);
            warning_shown = 1;
        }
    }

    /* Close UART before exiting */
    if (close(fd) == -1) {
        perror("close failed");
        exit_status = 1;
    }

    printf("\nGPS Monitor stopped.\n");
    printf("UART port closed.\n");

    return exit_status;
}

