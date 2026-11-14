# On the first console: start X, and if the app ever exits, start it again.
if [ "$(tty)" = /dev/tty1 ]; then
    while true; do
        startx /root/.xinitrc >/dev/null 2>&1
        sleep 1
    done
fi
