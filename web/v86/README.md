# v86 для браузерного лаунчера

Эти файлы использует корневой `index.html`:

- `libv86.js` и `v86.wasm`: npm-пакет `v86@0.5.424`;
- `seabios.bin` и `vgabios.bin`: `copy/v86`, commit `f3d4472a9c934b9ad78a311f5849ba711a296d23`;
- `LICENSE`: лицензия v86 BSD 2-Clause.

Запуск: из корня репозитория `python3 -m http.server 8080`, затем открыть `/index.html` и выбрать `disk_img/dimos.img`.
