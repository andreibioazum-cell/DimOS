# Same as .bash_profile, for the zsh root may get.
if [ "$(tty)" = /dev/tty1 ]; then
    while true; do
        startx /root/.xinitrc
        status=$?
        if [ "$status" -ne 0 ]; then
            printf '\nDimOS: the graphical session failed (status %s), retrying in 5 s.\n' "$status"
            sleep 5
        fi
    done
fi
