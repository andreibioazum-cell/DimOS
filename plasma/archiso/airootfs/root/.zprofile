# Start Snake on the first console and restart it after every quit.
if [ "$(tty)" = /dev/tty1 ]; then
    while true; do /usr/local/bin/snake; done
fi
