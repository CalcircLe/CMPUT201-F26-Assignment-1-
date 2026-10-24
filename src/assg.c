#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fixed array limits */
#define MAX_ROUTES 512
#define MAX_STOPS 512
#define MAX_NAME 256
#define MAX_PATH 1024
#define MAX_RECORDS 1000000

typedef struct {
    int id;
    char name[MAX_NAME];
} Stop;

typedef struct {
    char id[32];
    char direction;
    int stop_count;
    Stop stops[MAX_STOPS];
} Route;

/* load one schedule */
int read_route(const char path[], Route routes[], int index)
{
    int filename_start = 0;
    char filename[MAX_PATH];
    char line[1024];
    char extra;
    FILE *file;

    /* filename without folders */
    for (int i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') filename_start = i + 1;
    }
    int j = 0;
    while (path[filename_start + j] != '\0') {
        filename[j] = path[filename_start + j];
        j++;
    }
    filename[j] = '\0';
    /* route ID from filename, including X */
    if (sscanf(filename, "route_%31[^_]_%c", routes[index].id,
               &routes[index].direction) != 2 ||
        strchr("EWNS", routes[index].direction) == NULL) {
        fprintf(stderr, "Invalid schedule filename: %s\n", path);
        return 0;
    }

    file = fopen(path, "r");
    if (file == NULL) {
        fprintf(stderr, "Cannot open schedule: %s\n", path);
        return 0;
    }

    /* stop count from header */
    if (fgets(line, sizeof(line), file) == NULL ||
        sscanf(line, "%*s %*[^,], %d Stops", &routes[index].stop_count) != 1 ||
        routes[index].stop_count < 1 || routes[index].stop_count > MAX_STOPS) {
        fprintf(stderr, "Invalid schedule header: %s\n", path);
        fclose(file);
        return 0;
    }

    for (int i = 0; i < routes[index].stop_count; i++) {
        if (fgets(line, sizeof(line), file) == NULL ||
            sscanf(line, "%d,%255[^\r\n]%c", &routes[index].stops[i].id,
                   routes[index].stops[i].name, &extra) < 2) {
            fprintf(stderr, "Invalid stop data: %s\n", path);
            fclose(file);
            return 0;
        }
    }

    fclose(file);
    return 1;
}

typedef struct {
    int year, month, day, hour, minute;
    char type;
    int stop;
    long long time; /* minutes since 0001-01-01 */
} Tap;

typedef struct {
    Tap entry, exit;
    int guessed;
    int order; /* tie breaker for equal entry times */
} Trip;

/* leap year check */
int leap_year(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

/* parse one tap */
int parse_tap(const char line[], Tap taps[], int index)
{
    int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    char period[3];
    int used = 0; /* start of stop field */
    long long days = 0;
    if (sscanf(line, "%2d-%2d-%4d,%2d:%2d %2[APM],%c,%n",
               &taps[index].month, &taps[index].day, &taps[index].year, &taps[index].hour,
               &taps[index].minute, period, &taps[index].type, &used) != 7 || used == 0 ||
        (strcmp(period, "AM") != 0 && strcmp(period, "PM") != 0) ||
        taps[index].year < 1 || taps[index].month < 1 || taps[index].month > 12 ||
        taps[index].hour < 1 || taps[index].hour > 12 || taps[index].minute < 0 || taps[index].minute > 59 ||
        strchr("EXM", taps[index].type) == NULL) {
        return 0;
    }
    /* month length and leap day */
    days_in_month[1] += leap_year(taps[index].year);
    if (taps[index].day < 1 || taps[index].day > days_in_month[taps[index].month - 1]) {
        return 0;
    }
    taps[index].stop = 0;
    /* empty stop for M, numeric stop for E/X */
    if (taps[index].type == 'M') {
        if (line[used] != '\0') return 0;
    } else {
        int digits = 0;
        for (int i = used; line[i] != '\0'; i++) {
            if (line[i] < '0' || line[i] > '9' || digits == 9) return 0;
            taps[index].stop = taps[index].stop * 10 + line[i] - '0';
            digits++;
        }
        if (digits == 0) return 0;
    }
    /* 12 AM -> 0, 12 PM -> 12 */
    taps[index].hour = taps[index].hour % 12 + (strcmp(period, "PM") == 0 ? 12 : 0);
    /* total minutes for sorting and durations */
    for (int year = 1; year < taps[index].year; year++) days += 365 + leap_year(year);
    for (int month = 1; month < taps[index].month; month++) days += days_in_month[month - 1];
    days += taps[index].day - 1;
    taps[index].time = (days * 24 + taps[index].hour) * 60 + taps[index].minute;
    return 1;
}

/* stop name, or NULL if absent */
const char *stop_name(const Route routes[], int index, int id)
{
    for (int i = 0; i < routes[index].stop_count; i++) {
        if (routes[index].stops[i].id == id) return routes[index].stops[i].name;
    }
    return NULL;
}

/* stop lookup across all routes */
int known_stop(const Route routes[], int count, int id)
{
    for (int i = 0; i < count; i++) {
        if (stop_name(routes, i, id) != NULL) return 1;
    }
    return 0;
}

/* insertion sort by entry time */
void sort_trips(Trip trips[], int count)
{
    for (int i = 1; i < count; i++) {
        Trip current = trips[i];
        int j = i - 1;
        /* shift later trips right */
        while (j >= 0 &&
               (trips[j].entry.time > current.entry.time ||
                (trips[j].entry.time == current.entry.time &&
                 trips[j].order > current.order))) {
            trips[j + 1] = trips[j];
            j--;
        }
        trips[j + 1] = current;
    }
}

/* completed history only, no guessed exits */
int guess_exit(const Trip trips[], int current)
{
    int best = 0, best_count = 0;
    for (int i = 0; i < current; i++) {
        int count = 0;
        if (trips[i].guessed || trips[i].entry.stop != trips[current].entry.stop ||
            trips[i].entry.hour != trips[current].entry.hour ||
            trips[i].exit.time > trips[current].entry.time) continue;
        /* exit frequency for this entry stop and hour */
        for (int j = 0; j < current; j++) {
            if (!trips[j].guessed && trips[j].entry.stop == trips[current].entry.stop &&
                trips[j].entry.hour == trips[current].entry.hour &&
                trips[j].exit.time <= trips[current].entry.time &&
                trips[j].exit.stop == trips[i].exit.stop) count++;
        }
        /* smaller stop ID on a tie */
        if (count > best_count || (count == best_count && trips[i].exit.stop < best)) {
            best_count = count;
            best = trips[i].exit.stop;
        }
    }
    return best;
}

/* chosen route index, or -1 on failure */
int select_route(const Route routes[], int count, Trip trip)
{
    int candidates[MAX_ROUTES], matches = 0;
    char line[128];
    /* choices in list-file order */
    for (int i = 0; i < count; i++) {
        if (stop_name(routes, i, trip.entry.stop) != NULL &&
            stop_name(routes, i, trip.exit.stop) != NULL) candidates[matches++] = i;
    }
    if (matches == 0) {
        puts("No matching route");
        return -1;
    }
    if (matches == 1) return candidates[0];
    puts("Ambiguous trip detected:");
    printf("Entry: %d (%s)\n", trip.entry.stop,
           stop_name(routes, candidates[0], trip.entry.stop));
    printf("Exit: %d (%s)\n", trip.exit.stop,
           stop_name(routes, candidates[0], trip.exit.stop));
    puts("Possible routes:");
    for (int i = 0; i < matches; i++) {
        printf("%d. %s %c\n", i + 1, routes[candidates[i]].id,
               routes[candidates[i]].direction);
    }
    /* retry invalid choices; stop at EOF */
    while (1) {
        int selection;
        char extra;
        printf("Select route (1-%d): \n", matches);
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) return -1;
        if (sscanf(line, "%d %c", &selection, &extra) == 1 &&
            selection >= 1 && selection <= matches) return candidates[selection - 1];
    }
}

/* ISO timestamp, seconds always 00 */
void write_timestamp(FILE *file, Tap tap)
{
    fprintf(file, "%04d-%02d-%02dT%02d:%02d:00", tap.year,
            tap.month, tap.day, tap.hour, tap.minute);
}

/* tap history -> chronological trips */
int analyze(const char path[], const Route routes[], int route_count)
{
    FILE *input = fopen(path, "r");
    FILE *output = NULL;
    /* static storage for the large array */
    static Trip trips[MAX_RECORDS / 2];
    char line[1024], extra;
    int records, count = 0, success = 0;
    Tap exit = {0};
    if (input == NULL) {
        fprintf(stderr, "Cannot open tap history: %s\n", path);
        return 0;
    }
    if (fgets(line, sizeof(line), input) == NULL ||
        sscanf(line, "%d %c", &records, &extra) != 1 || records < 0 ||
        records > MAX_RECORDS || records % 2 != 0) goto invalid;

    /* pair X/E or M/E before sorting; possible overlapping trips */
    for (int i = 0; i < records; i++) {
        Tap parsed[1];
        if (fgets(line, sizeof(line), input) == NULL) goto invalid;
        line[strcspn(line, "\r\n")] = '\0';
        if (!parse_tap(line, parsed, 0)) goto invalid;
        Tap tap = parsed[0];
        if (tap.type != 'M' && !known_stop(routes, route_count, tap.stop)) {
            puts("Invalid stop ID");
            goto cleanup;
        }
        if (i % 2 == 0) {
            if (tap.type != 'X' && tap.type != 'M') goto invalid;
            exit = tap;
        } else {
            if (tap.type != 'E' || tap.time > exit.time) goto invalid;
            trips[count].entry = tap;
            trips[count].exit = exit;
            trips[count].guessed = exit.type == 'M';
            /* reverse file order for tied times */
            trips[count].order = -count;
            count++;
        }
    }
    /* trailing blank lines only */
    while (fgets(line, sizeof(line), input) != NULL) {
        if (strspn(line, " \t\r\n") != strlen(line)) goto invalid;
    }
    sort_trips(trips, count);
    output = fopen("output.txt", "w");
    if (output == NULL) goto cleanup;
    for (int i = 0; i < count; i++) {
        /* guess the stop, keep the M timestamp */
        if (trips[i].guessed) {
            trips[i].exit.stop = guess_exit(trips, i);
            if (trips[i].exit.stop == 0) {
                puts("Cannot guess missing tap");
                goto cleanup;
            }
        }
        int selected = select_route(routes, route_count, trips[i]);
        if (selected < 0) goto cleanup;
        /* duration in minutes -> hours and minutes */
        long long duration = trips[i].exit.time - trips[i].entry.time;
        fprintf(output, "%s %c,%s,%s,", routes[selected].id, routes[selected].direction,
                stop_name(routes, selected, trips[i].entry.stop),
                stop_name(routes, selected, trips[i].exit.stop));
        write_timestamp(output, trips[i].entry);
        fputc(',', output);
        write_timestamp(output, trips[i].exit);
        fprintf(output, ",%lld:%02lld:00%s\n", duration / 60, duration % 60,
                trips[i].guessed ? ",Guessed" : "");
    }
    success = 1;
    goto cleanup;
invalid:
    puts("Invalid data format");
cleanup:
    /* file cleanup */
    fclose(input);
    if (output != NULL && fclose(output) != 0) success = 0;
    return success;
}

int main(int argc, char *argv[])
{
    static Route routes[MAX_ROUTES];
    char path[MAX_PATH];
    int route_count = 0;
    int success = 1;
    FILE *list;

    if (argc != 2 && argc != 3) {
        fprintf(stderr, "Usage: %s <list file> [tap history]\n", argv[0]);
        return EXIT_FAILURE;
    }

    list = fopen(argv[1], "r");
    if (list == NULL) {
        fprintf(stderr, "Cannot open list file: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    /* one schedule path per line */
    while (fgets(path, sizeof(path), list) != NULL) {
        path[strcspn(path, "\r\n")] = '\0';
        if (path[0] == '\0') {
            continue;
        }
        if (route_count == MAX_ROUTES ||
            !read_route(path, routes, route_count)) {
            success = 0;
            break;
        }
        route_count++;
    }
    if (ferror(list)) {
        success = 0;
    }
    fclose(list);

    /* two file arguments: FS; one: MS */
    if (success && argc == 3) {
        success = analyze(argv[2], routes, route_count);
    } else if (success) {
        printf("Processing %d schedule files...\n", route_count);
        for (int i = 0; i < route_count; i++) {
            printf("schedule #%d is route %s it has %d stops and is in the %c direction\n",
                   i + 1, routes[i].id, routes[i].stop_count,
                   routes[i].direction);
        }
    }

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
