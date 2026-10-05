#!/usr/bin/env bash
# Point test_remote at this runner over the real ssh: a throwaway key made for
# this job, authorized only from the loopback address with every forwarding
# and the pty refused (`restrict`), and sshd started if it is not running.
# On success it exports BROMUX_TEST_SSH* to the later steps; when sshd cannot
# be brought up it says why and exports nothing, so test_remote skips (exit
# 77) and the job summary's skip list shows the reason.
#
#   ssh-localhost.sh <build-dir>   (bromux at <build-dir>/bromux,
#                                   mux_child at <build-dir>/tests/mux_child)
set -euo pipefail

build=$(cd "$1" && pwd)
USER=$(id -un)
key="$HOME/.ssh/bromux_ci_ed25519"

mkdir -p "$HOME/.ssh"
chmod 700 "$HOME/.ssh"
rm -f "$key" "$key.pub"
ssh-keygen -q -t ed25519 -N '' -C "bromux-ci-$GITHUB_RUN_ID" -f "$key"
printf 'restrict,from="127.0.0.1,::1" %s\n' "$(cat "$key.pub")" >> "$HOME/.ssh/authorized_keys"
chmod 600 "$HOME/.ssh/authorized_keys"
# sshd's StrictModes refuses keys under a group- or world-writable home.
chmod go-w "$HOME"

up() { nc -z 127.0.0.1 22 >/dev/null 2>&1; }

case "$(uname -s)" in
    Linux)
        if ! command -v sshd >/dev/null 2>&1 && [ ! -x /usr/sbin/sshd ]; then
            sudo apt-get install -y --no-install-recommends openssh-server >/dev/null
        fi
        up || sudo systemctl start ssh.socket ssh.service 2>/dev/null || sudo systemctl start ssh || true
        ;;
    Darwin)
        # Remote Login. systemsetup needs Full Disk Access on recent macOS;
        # loading the launchd job directly does not.
        up || sudo launchctl load -w /System/Library/LaunchDaemons/ssh.plist 2>/dev/null || true
        up || sudo launchctl enable system/com.openssh.sshd 2>/dev/null || true
        up || sudo launchctl kickstart -k system/com.openssh.sshd 2>/dev/null || true
        ;;
esac

for _ in 1 2 3 4 5 6 7 8 9 10; do up && break; sleep 1; done
if ! up; then
    echo "test_remote: SKIPPED: sshd could not be started on this runner ($(uname -s)); test_remote will skip" | tee -a "$GITHUB_STEP_SUMMARY"
    exit 0
fi

ssh_args="-i $key -oIdentitiesOnly=yes -oIdentityAgent=none -oForwardAgent=no -oStrictHostKeyChecking=accept-new -oUserKnownHostsFile=$HOME/.ssh/bromux_ci_known_hosts -oConnectTimeout=10"
# shellcheck disable=SC2086
if ! ssh $ssh_args -oBatchMode=yes -T "$USER@127.0.0.1" true; then
    echo "test_remote: SKIPPED: sshd is up but refused the job key ($(uname -s)); test_remote will skip" | tee -a "$GITHUB_STEP_SUMMARY"
    exit 0
fi

{
    echo "BROMUX_TEST_SSH=$USER@127.0.0.1"
    echo "BROMUX_TEST_SSH_ARGS=$ssh_args"
    echo "BROMUX_TEST_SSH_BROMUX=$build/bromux"
    echo "BROMUX_TEST_SSH_CHILD=$build/tests/mux_child"
} >> "$GITHUB_ENV"
echo "test_remote will run against $USER@127.0.0.1 over ssh"
