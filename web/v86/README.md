# Архивные файлы v86

Эти vendored-файлы остались для истории и воспроизводимости старого браузерного лаунчера:

- `libv86.js` и `v86.wasm`: npm-пакет `v86@0.5.424`;
- `seabios.bin` и `vgabios.bin`: `copy/v86`, commit `f3d4472a9c934b9ad78a311f5849ba711a296d23`;
- `LICENSE`: лицензия v86 BSD 2-Clause.

Корневой `index.html` удалён, текущая сборка и release-артефакты эти файлы не используют. Для запуска `disk_img/dimos.img` применяется внешний [v86 debug](https://copy.sh/v86/debug.html), как описано в корневом `README.md`.
