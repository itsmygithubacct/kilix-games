# sandbox-probe.sh - runs INSIDE a puzzllm sandbox (fed to `bash -s`).
#
#   bash -s -- MODE OUTSIDE HOST_PORT HOST_HOME < sandbox-probe.sh
#
# MODE is agent or shell. OUTSIDE is a host folder outside the game's state,
# HOST_PORT a TCP port listening on the host's loopback, HOST_HOME the host
# user's home. Prints one line per check; exit status is the failure count.
mode=$1 outside=$2 host_port=$3 host_home=$4
fails=0
ok()   { printf 'ok   %s %s\n' "$mode" "$*"; }
fail() { printf 'FAIL %s %s\n' "$mode" "$*"; fails=$((fails + 1)); }
must_fail_write() {
    if ( : > "$1" ) 2>/dev/null; then fail "wrote $1"; else ok "cannot write $1"; fi
}

[ "$PWD" = /kilix/workspace ] && ok "cwd is the workspace" || fail "cwd is $PWD"

# writes outside the workspace fail
must_fail_write "$outside/escape-$mode"
must_fail_write "$outside/../escape-$mode"
must_fail_write /usr/escape
must_fail_write /etc/escape
must_fail_write /escape
must_fail_write /opt/escape
must_fail_write ../../../escape
[ "$mode" = agent ] && must_fail_write /opt/puzzllm/bin/omp

# the workspace itself is writable, in both sandboxes (owner's decision)
if echo "$mode" > "./written-by-$mode" 2>/dev/null; then ok "writes the workspace"
else fail "cannot write the workspace"; fi

if [ "$mode" = agent ]; then
    if : > "$HOME/.omp/agent/probe" 2>/dev/null; then ok "writes its omp folder"
    else fail "cannot write its omp folder"; fi
else
    [ ! -e "$HOME/.omp" ] && ok "no omp folder in the shell" || fail "shell sees ~/.omp"
fi

# nothing of the host's home is visible
[ ! -e "$host_home" ] && ok "host home not visible" || fail "host home $host_home is visible"

# no network: not the host's loopback, not anywhere else
if timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/$host_port" 2>/dev/null; then
    fail "reached the host loopback port $host_port"
else
    ok "host loopback unreachable"
fi
if timeout 3 bash -c 'exec 3<>/dev/tcp/192.0.2.1/80' 2>/dev/null; then
    fail "reached an outside address"
else
    ok "outside network unreachable"
fi

# mount audit: every writable mount is one the design allows
allowed="/kilix/workspace"
[ "$mode" = agent ] && allowed="$allowed /kilix/.omp/agent"
while read -r _ _ _ _ point opts rest; do
    fstype=${rest#*- }
    fstype=${fstype%% *}
    case ",$opts," in *,rw,*) ;; *) continue ;; esac
    case "$point" in
    /) fail "root is writable" ;;
    /dev|/dev/*|/proc|/proc/*) ;;
    /tmp|/home|/kilix)
        [ "$fstype" = tmpfs ] || fail "$point is writable $fstype, not tmpfs" ;;
    *)
        case " $allowed " in
        *" $point "*) ;;
        *) fail "writable mount $point ($fstype)" ;;
        esac ;;
    esac
done < /proc/self/mountinfo
ok "mount audit done"

exit "$fails"
