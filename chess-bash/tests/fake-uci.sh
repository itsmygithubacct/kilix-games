#!/bin/sh

# Small deterministic UCI peer for the engine lifecycle regression harness.
# Behavior is selected per process through FAKE_UCI_MODE.
mode=${FAKE_UCI_MODE:-normal}
log_file=${FAKE_UCI_LOG:-/dev/null}
go_count=0

log_line()
{
    printf '%s\n' "$1" >>"$log_file"
}

trap 'exit 0' HUP INT TERM

while IFS= read -r command; do
    log_line "$command"
    case $command in
    uci)
        [ "$mode" = eof-start ] && exit 0
        printf '%s\n' \
            'id name Chess Bash Fake UCI' \
            'id author lifecycle test' \
            'option name Threads type spin default 1 min 1 max 1' \
            'option name Hash type spin default 1 min 1 max 64' \
            'option name Skill Level type spin default 10 min 0 max 20' \
            'uciok'
        ;;
    isready)
        printf '%s\n' 'readyok'
        ;;
    go*)
        go_count=$((go_count + 1))
        log_line "go $go_count"
        if [ "$mode" = eof-search ]; then
            exit 0
        elif [ "$mode" = cancel ] && [ "$go_count" -eq 1 ]; then
            # An outstanding search produces no reply until `stop`, while
            # this command loop remains responsive to stdin.
            :
        elif [ "$mode" = cancel ]; then
            printf '%s\n' 'bestmove d7d5'
        else
            printf '%s\n' 'bestmove e2e4'
        fi
        ;;
    stop)
        printf '%s\n' 'bestmove e7e5'
        ;;
    quit)
        exit 0
        ;;
    esac
done
