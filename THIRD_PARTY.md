# Сторонние компоненты

Протокол подписи запросов API адаптирован из
[amdray/yandex_music_psp](https://github.com/amdray/yandex_music_psp),
коммит `a15bd4613873845397e502945419f788fb8031d4`.
Copyright (c) 2026 amdray; лицензия MIT сохранена в `LICENSE.upstream`.

- cJSON 1.7.19 — исходники и лицензия MIT в `vendor/cjson`.
- minimp3 — исходники и документ CC0 в `vendor/minimp3`.
  Коммит `ea99364f61c14656440e8d77e9c233ccf3124633`.
  В `minimp3_ex.h` добавлена возможность переопределить `MINIMP3_IO_SIZE`;
  плеер использует буфер 32 КиБ вместо стандартных 128 КиБ.
- Сертификаты CA Mozilla получены с https://curl.se/ca/cacert.pem
  07.10.2026; происхождение и лицензия указаны в заголовке PEM.
- Сборка для Nintendo 3DS использует devkitPro libctru, citro2d, citro3d,
  curl, mbedTLS и zlib. Их лицензии поставляются с пакетами devkitPro.

Названия и товарные знаки Яндекса принадлежат их владельцам. Проект неофициальный.
