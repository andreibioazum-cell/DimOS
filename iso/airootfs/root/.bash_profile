# On the first console: start X. If X or the session ever fails, say so on the
# console (the only text this system can print) and try again instead of
# leaving the machine on a black screen.
if [ "$(tty)" = /dev/tty1 ]; then
    while true; do
        startx /root/.xinitrc
        status=$?
        if [ "$status" -ne 0 ]; then
            printf '\nDimOS: the graphical session failed (status %s), retrying in 5 s.\n' "$status"
            printf 'DimOS: see /var/log/Xorg.0.log and /var/log/dimos.log\n'
            sleep 5
        fi
    done
fi
