# Same as .bash_profile, for the zsh the releng profile gives root.
if [ "$(tty)" = /dev/tty1 ]; then
    while true; do
        startx /root/.xinitrc >/dev/null 2>&1
        sleep 1
    done
fi
