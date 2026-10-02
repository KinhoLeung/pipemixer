#include <unistd.h>
#include <getopt.h>

#include <ncurses.h>
#include <spa/utils/string.h>

#include "log.h"
#include "config.h"
#include "cli.h"
#include "macros.h"
#include "eventloop.h"
#include "tui/tui.h"
#include "pw/common.h"

struct pw_main_loop *main_loop = NULL;
struct pw_loop *event_loop = NULL;

static void events_fd_handler(void *_, int _, uint32_t _) {
    events_dispatch();
}

static void bad_signal_handler(int sig) {
    /* restore terminal state before crashing */
    endwin();
    tui_disable_button_motion_tracking();
    raise(sig);
}

static void exit_signal_handler(int sig) {
    INFO("caught signal %d, stopping main loop", sig);
    pw_main_loop_quit(main_loop);
}

void print_help_and_exit(FILE *stream, int exit_status) {
    const char help_string[] =
        "pipemixer - pipewire volume control\n"
        "\n"
        "usage:\n"
        "    pipemixer [OPTIONS]\n"
        "    pipemixer [OPTIONS] COMMAND [ARGUMENTS]\n"
        "\n"
        "command line options:\n"
        "    -c, --config     path to configuration file\n"
        "    -v, --validate   validate config file and exit 1 on errors\n"
        "    -l, --loglevel   one of TRACE, DEBUG, INFO, WARN, ERROR, QUIET\n"
        "    -L, --log-fd     write log to this fd (must be open for writing)\n"
        "    -C, --color      force logging with colors\n"
        "    -V, --version    print version information\n"
        "    -j, --json       JSON output for query commands\n"
        "    -t, --timeout    command timeout in milliseconds (default: 5000)\n"
        "    -h, --help       print this help message and exit\n"
        "\n"
        "commands (TARGET is an exact name, id:N, or node serial:N):\n"
        "    list [nodes|devices]\n"
        "    get-volume TARGET [CHANNEL]\n"
        "    set-volume TARGET PERCENT [CHANNEL]\n"
        "    get-mute TARGET\n"
        "    set-mute TARGET on|off|toggle\n"
        "    get-default sink|source\n"
        "    set-default TARGET\n"
        "    list-routes TARGET\n"
        "    set-route TARGET INDEX\n"
        "    set-target STREAM DESTINATION|default\n"
        "    list-profiles DEVICE\n"
        "    set-profile DEVICE INDEX\n"
        "\n"
        "exit codes: 0 success, 1 PipeWire/timeout error, 2 usage error, 3 target unavailable\n";

    fputs(help_string, stream);
    exit(exit_status);
}

void print_version_and_exit(FILE *stream, int exit_status) {
    const char version_string[] =
        "pipemixer version " PIPEMIXER_VERSION
    #ifdef PIPEMIXER_GIT_TAG
        ", git tag " PIPEMIXER_GIT_TAG
    #endif
    #ifdef PIPEMIXER_GIT_BRANCH
        ", git branch " PIPEMIXER_GIT_BRANCH
    #endif
        "\n"
    ;

    fputs(version_string, stream);
    exit(exit_status);
}

int main(int argc, char **argv) {
    int retcode = 0;

    const char *config_path = NULL;
    bool validate_config = false;
    FILE *log_stream = NULL;
    int log_fd = -1;
    enum log_loglevel loglevel = LOG_DEBUG;
    bool log_force_colors = false;
    struct cli_request cli_request = { .timeout_ms = 5000 };
    bool cli_options = false;

    /* for easily attaching gdb */
    uint32_t startup_sleep;
    if (spa_atou32(getenv("PIPEMIXER_STARTUP_SLEEP"), &startup_sleep, 10)) {
        sleep(startup_sleep);
    }

    static const char shortopts[] = "+c:L:l:vCVhjt:";
    static const struct option longopts[] = {
        { "config",      required_argument, NULL, 'c' },
        { "log-fd",      required_argument, NULL, 'L' },
        { "loglevel",    required_argument, NULL, 'l' },
        { "validate",    no_argument,       NULL, 'v' },
        { "color",       no_argument,       NULL, 'C' },
        { "version",     no_argument,       NULL, 'V' },
        { "help",        no_argument,       NULL, 'h' },
        { "json",        no_argument,       NULL, 'j' },
        { "timeout",     required_argument, NULL, 't' },
        { 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, shortopts, longopts, NULL)) > 0) {
        switch (c) {
        case 'c':
            config_path = optarg;
            break;
        case 'v':
            validate_config = true;
            break;
        case 'L':
            if (!spa_atoi32(optarg, &log_fd, 10)) {
                fprintf(stderr, "failed to convert %s to integer\n", optarg);
                return 2;
            }
            break;
        case 'l':
            loglevel = log_str_to_loglevel(optarg);
            if (loglevel == LOG_INVALID) {
                fprintf(stderr, "%s is not a valid loglevel\n", optarg);
                return 2;
            }
            break;
        case 'C':
            log_force_colors = true;
            break;
        case 'j':
            cli_request.json = true;
            cli_options = true;
            break;
        case 't':
            if (!spa_atou32(optarg, &cli_request.timeout_ms, 10)
                || cli_request.timeout_ms < 1 || cli_request.timeout_ms > 600000) {
                fprintf(stderr, "pipemixer: timeout must be 1..600000 ms\n");
                return 2;
            }
            cli_options = true;
            break;
        case 'V':
            print_version_and_exit(stdout, 0);
            break;
        case 'h':
            print_help_and_exit(stdout, 0);
            break;
        default:
            print_help_and_exit(stderr, 2);
            break;
        }
    }

    if (log_fd > 0) {
        log_stream = fdopen(log_fd, "w");
        if (log_stream == NULL) {
            fprintf(stderr, "failed to fdopen() fd %d: %s\n", log_fd, strerror(errno));
            exit(1);
        }

        log_init(log_stream, loglevel, log_force_colors);
    }

    /* needed for unicode support in ncurses and correct unicode handling in config */
    setlocale(LC_ALL, "");

    const bool cli_mode = optind < argc;
    if (cli_mode || cli_options) {
        setlocale(LC_NUMERIC, "C");
        int parse_status = cli_parse(argc - optind, argv + optind, &cli_request);
        if (parse_status) return parse_status;
    }

    bool config_valid = (cli_mode && !config_path && !validate_config)
                        ? true : load_config(config_path);
    if (validate_config) {
        return !config_valid;
    }
    if (cli_mode && !config_valid) return 1;

    pw_init(NULL, NULL);

    main_loop = pw_main_loop_new(NULL);
    if (!main_loop) {
        fprintf(stderr, "failed to create event loop\n");
        retcode = 1;
        goto cleanup;
    }
    event_loop = pw_main_loop_get_loop(main_loop);

    int events_fd = events_global_init();
    pw_loop_add_io(event_loop, events_fd, POLL_IN, false, events_fd_handler, NULL);

    /* naming is unfortunate */
    if (!pipewire_init()) {
        fprintf(stderr, "pipemixer: failed to connect to pipewire\n");
        retcode = 1;
        goto cleanup;
    }

    if (cli_mode) {
        retcode = cli_run(&cli_request);
        goto cleanup;
    }

    /* setup crash handler before initialising ncurses */
    static const int bad_signals[] = {
        SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS,
    };
    for (unsigned i = 0; i < SIZEOF_ARRAY(bad_signals); i++) {
        sigaction(bad_signals[i], &(struct sigaction){
            .sa_handler = bad_signal_handler,
            .sa_flags = SA_NODEFER | SA_RESETHAND | SA_RESTART,
        }, NULL);
    }

    static const int exit_signals[] = {
        SIGTERM, SIGINT,
    };
    for (unsigned i = 0; i < SIZEOF_ARRAY(exit_signals); i++) {
        sigaction(exit_signals[i], &(struct sigaction){
            .sa_handler = exit_signal_handler,
            .sa_flags = SA_RESTART,
        }, NULL);
    }

    tui_init();

    TRACE("entering main loop");
    pw_main_loop_run(main_loop);
    TRACE("leaving main loop");

cleanup:
    tui_cleanup();
    pipewire_cleanup();

    /* see https://invisible-island.net/ncurses/man/curs_memleaks.3x.html */
    if (cli_mode) return retcode;
    exit_curses(retcode);
}

