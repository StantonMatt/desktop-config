#!/bin/bash
# Install the resident overlay daemon as Meta+Shift+S and Print. The Spectacle menu entry remains stock.
set -euo pipefail
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=shortcut-lib.sh
source "$source_dir/shortcut-lib.sh"
scratch="$HOME/.cache/agent-scratch/desktop-config"
state_dir="$HOME/.local/state/quick-region-overlay"
state_file="$state_dir/handover"
mode_file="$state_dir/previous-mode"
manifest_file="$state_dir/manifest"
recover=
work=
trap on_exit EXIT

need_tools cmake c++ qdbus6 busctl jq kbuildsycoca6 systemctl systemd-run heavy cmp perl flock
take_lock
work=$(mkdir -p "$scratch" && mktemp -d "$scratch/overlay-install.XXXXXX")
if [[ ! -f /usr/include/KF6/KGuiAddons/KSystemClipboard || ! -f /usr/include/LayerShellQt/window.h || ! -f /usr/include/KF6/KGlobalAccel/KGlobalAccel ]]; then
    fail 'Missing dependency: sudo -n apt-get install --no-install-recommends libkf6guiaddons-dev liblayershellqtinterface-dev libkf6globalaccel-dev'
fi
cmake -S "$source_dir" -B "$work/cmake" -DCMAKE_BUILD_TYPE=Release >/dev/null
heavy cmake --build "$work/cmake" --parallel 2 --target quick-region-overlay

# 1. Prechecks. Every file that a forward-only step can delete is compared with an immutable backup first.
previous=$(launcher_mode)
handover_state=
[[ -f "$state_file" ]] && handover_state=$(cat -- "$state_file")
valid_marker "$handover_state" ||
    fail "The handover state marker is invalid; remove $state_file after checking the desktop state. Nothing was changed."
recorded_mode=
if marker_pending "$handover_state" && [[ -f $mode_file ]]; then
    recorded_mode=$(cat -- "$mode_file")
    valid_mode "$recorded_mode" || fail "$mode_file is invalid; remove it after checking the desktop state. Nothing was changed."
fi
rollback_mode=$(recovery_mode "$previous" "$handover_state" "$recorded_mode")
check_backup "$rollback_mode"
[[ -f "$spectacle_fast_backup/$spectacle_override_relative" ]] || fail "The Spectacle override backup is missing; nothing was changed."
check_override
check_key_owner "$meta_shift_s_code" "$daemon_owner" "$launcher_owner"
check_key_owner "$print_code" "$daemon_owner" org.kde.spectacle.desktop/_launch
for relative in \
    "$launcher_relative" .config/systemd/user/quick-region-clipboard.service .local/bin/quick-region-screenshot \
    .local/libexec/quick-region-clipboard-service .local/libexec/quick-region-clipboard-service.py .local/libexec/quick-region-clipboard.py; do
    check_owned_file "$HOME/$relative" "$relative"
done
[[ ! -e "$HOME/.local/share/applications/quick-region-overlay-proto.desktop" ]] ||
    fail "$HOME/.local/share/applications/quick-region-overlay-proto.desktop is not an owned old-pipeline file; nothing was changed."

spectacle_print=0
checked spectacle_owns_print && spectacle_print=1
launcher_present=0
[[ -n $previous ]] && launcher_present=1
handover=$(handover_kind "$handover_state" "$launcher_present" "$spectacle_print")

# 2. Save the old files before any one is removed. Existing backups are never overwritten.
backup_file() {
    local relative=$1
    if [[ -e "$HOME/$relative" && ! -e "$spectacle_fast_backup/$relative" ]]; then
        mkdir -p -- "$(dirname -- "$spectacle_fast_backup/$relative")"
        cp -a -- "$HOME/$relative" "$spectacle_fast_backup/$relative"
    fi
}
for relative in .config/kglobalshortcutsrc .config/spectaclerc "$launcher_relative" "$spectacle_override_relative" \
    .config/systemd/user/quick-region-clipboard.service .local/bin/quick-region-screenshot \
    .local/libexec/quick-region-clipboard-service .local/libexec/quick-region-clipboard-service.py .local/libexec/quick-region-clipboard.py; do
    backup_file "$relative"
done
recover=$(rollback_command "$rollback_mode")
# A recorded mode outlives its handover only after an interruption between the two writes that complete it.
marker_pending "$handover_state" || rm -f -- "$mode_file"
changed=0
install_if_changed() {
    local mode=$1 source=$2 target=$3 relative=${3#"$HOME/"}
    if [[ -L $target ]] || ! cmp -s -- "$source" "$target"; then
        protect_managed_file "$relative"
        install -Dm"$mode" -- "$source" "$target"
        changed=1
    fi
    record_managed_file "$relative"
}
sed "s|@HOME@|$HOME|g" -- "$source_dir/quick-region-overlay.desktop" >"$work/quick-region-overlay.desktop"
install_if_changed 644 "$source_dir/quick-region-overlay.service" "$HOME/.config/systemd/user/quick-region-overlay.service"
install_if_changed 644 "$source_dir/org.local.QuickRegionOverlay.service" "$HOME/.local/share/dbus-1/services/org.local.QuickRegionOverlay.service"
install_if_changed 644 "$work/quick-region-overlay.desktop" "$HOME/.local/share/applications/quick-region-overlay.desktop"
protect_managed_file .local/share/applications/quick-region-overlay-proto.desktop
rm -f -- "$HOME/.local/share/applications/quick-region-overlay-proto.desktop"
rebuild_cache
systemctl --user daemon-reload
systemctl --user --quiet enable quick-region-overlay.service
# The binary goes last and the daemon restarts right after it. KWin authorizes ScreenShot2 by the caller's executable
# path, which reads "(deleted)" once the binary is replaced, so captures fail until the restart. A normal rerun keeps all
# saved keys; a failed or crash-looping unit is restarted too.
install_if_changed 755 "$work/cmake/quick-region-overlay" "$HOME/.local/libexec/quick-region-overlay"
if [[ $changed == 1 ]] || ! daemon_running; then restart_daemon; fi
fail_point after-files

# 3. Remove the private Spectacle override. A stock file is now selected by the desktop cache.
if [[ -e "$spectacle_override_entry" ]]; then
    check_override
    note_relogin
    rm -f -- "$spectacle_override_entry"
    rebuild_cache
fi

# 4. Check the daemon, then hand over the keys its old owners still hold. Retiring the launcher resets the action once
# so both defaults are acquired; Print taken from Spectacle is added to the action's current keys. See handover_kind.
status=$(daemon_status) || fail 'The daemon did not answer; see journalctl --user -u quick-region-overlay.service'
[[ $status != error:* ]] || fail "The daemon could not read its shortcut from KGlobalAccel ($status)."
fail_point after-daemon-start

if [[ -n $handover ]]; then
    printf '%s\n' "$handover" >"$state_file"
    if [[ -n $previous ]]; then
        # Recorded before the launcher goes, so a rerun after a failure still prints the right rollback command.
        printf '%s\n' "$previous" >"$mode_file"
        retire_launcher
        fail_point after-launcher-retire
    fi
    # Stock Spectacle may still have saved keys from before the override was removed. Remove only Print and
    # Meta+Shift+S without unregistering the action; an explicit none value or the remaining keys survives a later login
    # and ksycoca rescan.
    if checked action_has_any_key org.kde.spectacle.desktop _launch; then
        ensure_spectacle_action_print_removed
        fail_point after-print-retire
    fi
    if handover_needs_reset "$handover" "$status"; then
        systemctl --user stop quick-region-overlay.service
        unregister_shortcut quick-region-overlay capture-region
        restart_daemon
        status=$(daemon_status) || fail 'The daemon did not answer after the handover; see journalctl --user -u quick-region-overlay.service'
    fi
    if handover_needs_print "$handover" "$status"; then
        add_daemon_print
        wait_for 30 daemon_has_print || fail 'The daemon did not get Print after Spectacle gave it up.'
        status=$(daemon_status) || fail 'The daemon did not answer after the handover; see journalctl --user -u quick-region-overlay.service'
    fi
    handover_keys_present "$status" "$handover" || fail "The daemon did not get the required shortcuts after the handover ($status)."
    fail_point after-handover
    printf 'complete\n' >"$state_file"
    rm -f -- "$mode_file"
fi

# Older installs may have already completed the handover after unregistering Spectacle, leaving no action to update. Ensure
# that the action is present with Print removed even when there is no pending marker; this is a no-op once that state has
# been persisted and the daemon already owns the intended shortcuts.
if ! spectacle_config_no_print || ! spectacle_action_present || checked spectacle_owns_print; then
    ensure_spectacle_action_print_removed
    fail_point after-spectacle-empty
fi

# 5. Retire the old clipboard service and its files. The backup remains the rollback source.
if state_needs_stop "$(unit_state quick-region-clipboard.service)"; then systemctl --user stop quick-region-clipboard.service; fi
if [[ -e "$HOME/.config/systemd/user/quick-region-clipboard.service" ]]; then systemctl --user --quiet disable quick-region-clipboard.service; fi
rm -f -- "$HOME/.config/systemd/user/quick-region-clipboard.service" \
    "$HOME/.local/libexec/quick-region-clipboard-service" "$HOME/.local/libexec/quick-region-clipboard-service.py" \
    "$HOME/.local/libexec/quick-region-clipboard.py" "$HOME/.local/bin/quick-region-screenshot"
systemctl --user daemon-reload
systemctl --user reset-failed quick-region-clipboard.service 2>/dev/null || true
fail_point after-clipboard-service
recover=

case $status in
    registered:*) echo "Installed Quick Region Overlay (daemon PID $(systemctl --user show -P MainPID quick-region-overlay.service)), shortcuts ${status#registered:}." ;;
    none:*) echo "Installed Quick Region Overlay. Its shortcuts have no keys, and ${status#none:} owns a default; set them in System Settings → Shortcuts → Quick Region Overlay." ;;
    *) echo "Installed Quick Region Overlay. Its shortcuts have no keys (cleared in System Settings); set them in System Settings → Shortcuts → Quick Region Overlay." ;;
esac
print_relogin_note "The Spectacle override was removed. Log out and back in so Spectacle's other shortcuts, such as Meta+Shift+Print, stop using it."
