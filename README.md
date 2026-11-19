# CS3D

Las radios del mundo en una Nintendo 3DS. Busca en el directorio abierto de
[radio-browser.info](https://www.radio-browser.info/) (decenas de miles de
emisoras) y escucha la que elijas, con el título de lo que suena.

Las emisoras aparecen como puntos sobre un **globo terráqueo** con el mapa
de [OpenStreetMap](https://www.openstreetmap.org/copyright), desde el planeta
entero hasta las calles de una ciudad. Las que el
directorio sitúa (una de cada cuatro) van en sus coordenadas; el resto, en el
centro de su país, algo dispersas. Se cargan las 400 más populares (o las de
la búsqueda) en segundo plano.

## El mapa

[source/gfx/tiles.h](source/gfx/tiles.h) y [source/gfx/globe.h](source/gfx/globe.h):

- Las teselas raster de `tile.openstreetmap.org` (256×256 PNG) se piden al
  nivel de zoom que corresponde al tamaño del globo en pantalla, solo las
  visibles y empezando por el centro. Un hilo de baja prioridad las descarga
  (una conexión keep-alive), las decodifica con libpng a RGB565 en el formato
  en mosaico de la GPU y las guarda en `sdmc:/3ds/CS3D/tiles/z/x/y.png`: lo
  ya visto carga desde la tarjeta.
- En memoria caben 80 texturas (10 MB); los niveles 0–2 se piden al arrancar y
  no se descartan nunca. Mientras llega una tesela se estira la más cercana de
  un nivel inferior.
- La CPU proyecta una malla por tesela (más fina cuanto más curva se ve; un
  solo cuadrilátero al acercarse a una ciudad) y la GPU solo la texturiza
  ([globe.v.pica](source/gfx/globe.v.pica)), entre los dibujos de citro2d.
- Sin red, o hasta que llega la primera tesela, se ve el globo offline de
  puntos ([geodata.c](source/gfx/geodata.c)).
- Se cumple la [política de uso de teselas](https://operations.osmfoundation.org/policies/tiles/):
  User-Agent propio, nada de descargas masivas, caché local y la atribución
  «© OpenStreetMap contributors» en pantalla.

## Uso

- **Pantalla superior**: el globo. Al soltar el stick, la emisora más cercana
  al centro se centra y se enfoca (con su nombre al lado).
- **Pantalla inferior**: los datos de la enfocada (país, bitrate, votos,
  etiquetas, coordenadas) y lo que suena (título, estado, búfer, volumen).

| Botón | Acción |
|---|---|
| Stick | girar el globo |
| Cruceta izquierda / derecha | otra emisora del mismo sitio |
| L / R | alejar / acercar |
| A | escuchar la enfocada |
| B | parar |
| Y (o *Buscar*) | buscar por nombre con el teclado del sistema |
| X (o *Top*) | volver al top mundial |
| Cruceta arriba / abajo | volumen |
| START | salir |

Solo se listan emisoras que la consola puede reproducir: **MP3**, sin HLS, y
que el propio directorio no marca como caídas. Ordenadas por popularidad.

## Estructura

```
CS3D/
├── build.py                 compila todo source/, empaqueta .3dsx y .cia
├── source/
│   ├── main.c               la app: arranque, lista, entrada, dibujo, apagado
│   ├── app/
│   │   ├── radiobrowser.[ch] cliente del directorio: búsqueda, páginas, clics
│   │   └── stationmap.[ch]  las emisoras sobre el globo: giro, centrado, foco
│   ├── gfx/
│   │   ├── ui.[ch]          capa fina sobre citro2d (pantallas, texto, rectángulos)
│   │   ├── globe.[ch]       globo ortográfico: teselas OSM en la GPU, proyección
│   │   ├── globe.v.pica     shader de vértices de las teselas
│   │   ├── tiles.[ch]       teselas de OpenStreetMap: descarga, caché en SD, texturas
│   │   └── geodata.[ch]     máscara de tierra y centros de países (generado)
│   ├── audio/
│   │   ├── radio3ds.[ch]    radio en streaming: red -> búfer -> MP3 -> DSP
│   │   └── minimp3_impl.c   el decodificador minimp3 (descargado a deps/)
│   └── net/
│       ├── tls3ds.[ch]      TLS 1.2 (y TCP plano) sobre los sockets de libctru
│       ├── http3ds.[ch]     HTTP/1.1: GET/POST, chunked, streaming, keep-alive
│       ├── http_logic.h     utilidades puras (redirecciones, backoff)
│       ├── json3ds.[ch]     lector JSON mínimo sin reservas de memoria
│       ├── ca_bundle.[ch]   certificados raíz (generado)
│       ├── plat3ds.c        entropía (PS) y reloj para mbedTLS
│       └── mbedtls_config_3ds.h
├── tools/
│   ├── gen_ca_bundle.py     regenera ca_bundle.c a partir de hosts.txt
│   ├── gen_geodata.py       regenera geodata.c desde Natural Earth
│   └── hosts.txt            los hosts (y puertos) con los que habla tu app
├── assets/                  icono, banner, audio del banner, RSF de makerom
└── romfs/                   ficheros empaquetados como romfs:/
```

## Requisitos

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) con el grupo
  `3ds-dev` (devkitARM, libctru, citro3d, citro2d, 3dsxtool, bannertool,
  makerom, picasso) y los portlibs `3ds-libpng` y `3ds-zlib`.
- Python 3.12 o superior.
- Para el audio, la consola necesita `sdmc:/3ds/dspfirm.cdc` (el firmware
  del DSP). Se vuelca una sola vez con la app homebrew **DSP1**. Sin él se
  puede explorar el directorio, pero no escuchar.

## Compilar

```sh
python build.py --fetch-deps   # la primera vez: descarga y verifica mbedTLS y minimp3
python build.py                # siguientes veces, sin red
python build.py --debug        # -O0 y mensajes de error de mbedTLS legibles
python build.py --cia          # además, un .cia instalable con FBI
```

El resultado queda en `dist/CS3D/`. Copia esa carpeta a `SD:/3ds/` y
ábrela desde el Homebrew Launcher (o envíala con `3dslink`).

mbedTLS se compila una vez y se reutiliza mientras no cambien los flags ni
`mbedtls_config_3ds.h`.

## Personalizar

1. En `build.py`: `APP_TITLE`, `APP_DESCRIPTION`, `APP_PUBLISHER` y
   `APP_USER_AGENT` (radio-browser pide un User-Agent que identifique la
   app).
2. En `assets/template.rsf` (solo si generas `.cia`): `Title`, `ProductCode`
   y, sobre todo, `UniqueId` (uno aleatorio entre `0xF0000` y `0xFFFFF`).
3. Sustituye `assets/icon.png` (48×48), `assets/banner.png` (256×128) y
   `assets/banner.wav` (máximo ~3 s).
4. Pon tus hosts en `tools/hosts.txt` (`host` o `host:puerto`) y regenera
   los certificados (abajo).
5. Cualquier `.c` nuevo dentro de `source/` (y subcarpetas) se compila solo,
   y cada subcarpeta es ruta de includes.

## El directorio (radio-browser.info)

[source/app/radiobrowser.h](source/app/radiobrowser.h). Un hilo propio con
una sesión HTTPS keep-alive (un solo handshake para muchas peticiones):

```c
rb_init();                     /* tras tls_global_init() */
rb_search("");                 /* "" = top mundial; "rock" = por nombre */
rb_load_more();                /* página siguiente */
rb_click(uuid);                /* avisa al directorio de que se escuchó */

rb_info info;  rb_info_get(&info);          /* estado, nº de emisoras... */
rb_station st; rb_station_get(i, &st);      /* nombre, url, país, lat/lon */
```

- Usa los espejos `de1` y `de2` (y `all.api`, que reparte entre los vivos);
  si uno falla, pasa al siguiente. La lista viva está en `/json/servers`.
- Pide `codec=MP3&hidebroken=true&order=clickcount&reverse=true` y descarta
  las HLS. La paginación cuenta todas las entradas devueltas, también las
  descartadas, para no repetir emisoras.
- Cada búsqueda nueva invalida las páginas en vuelo de la anterior.
- Solo una de cada cuatro emisoras trae coordenadas; en el globo, el resto
  se sitúa por su país (`countrycode`, tabla de `geodata.c`).

## Usar HTTPS

```c
#include "http3ds.h"

http_reply r;
if (http_get("api.github.com", "/zen", NULL, &r) == 0) {
	/* r.status es el código HTTP; r.body termina en NUL. */
	printf("%d: %s\n", r.status, r.body);
	http_reply_free(&r);
} else {
	printf("error: %s\n", r.err);   /* DNS, TCP, TLS, timeout... */
}
```

Otras llamadas: `http_post_form`, `http_post_json`, `http_get_url` (URL
completa), `http_request_ex` (método y cabeceras libres),
`http_get_url_stream` (descargas grandes por trozos, con reanudación y
redirecciones) y `http_session_*` (keep-alive: varias peticiones al mismo
host pagando un solo handshake). Ver [source/net/http3ds.h](source/net/http3ds.h).

Para leer la respuesta: [source/net/json3ds.h](source/net/json3ds.h)
(`json_str`, `json_int`, `json_array_next`, `json_path`…).

### Reglas importantes

- **Orden de arranque**: `socInit` → `psInit` → `tls_global_init`. Apagado al
  revés: `tls_global_exit` → `psExit` → `socExit`. `psInit` es obligatorio: da
  la entropía del generador aleatorio de TLS.
- **Nunca en el hilo que dibuja.** Un handshake tarda segundos en una Old
  3DS. Usa un hilo de trabajo como en `main.c`, con prioridad un punto por
  debajo del hilo principal.
- **Varias conexiones, cada una en su hilo.** `tls3ds` serializa los
  handshakes y protege el generador aleatorio compartido, así que hilos
  distintos pueden tener cada uno su conexión abierta (la radio y las
  peticiones). Una misma conexión o `http_session` no se comparte entre
  hilos.
- **Pila del hilo**: 64 KB es seguro. Bajar mucho provoca desbordamientos
  durante la verificación RSA.
- **Salir rápido**: `tls_set_abort(true)` corta las esperas de red en
  ~100 ms para poder hacer `threadJoin` al cerrar.
- **Fecha y hora de la consola**: si están mal, todos los certificados
  fallan (el error lo dice explícitamente).
- Solo TLS 1.2. Las peticiones normales son siempre `https://` verificado.
  El streaming (`http_get_url_stream_ex`) admite además `http://` en claro y
  TLS sin verificar, con los flags `HTTP_STREAM_ALLOW_HTTP` y
  `HTTP_STREAM_NO_VERIFY`: lo usa la radio (ver abajo). Se admite puerto:
  `https://host:8066/stream`, o `http_get("host:8443", ...)`.

## Radio por internet

```c
#include "radio3ds.h"

char err[160];
if (!radio_init(err, sizeof(err)))       /* tras tls_global_init() */
	printf("%s\n", err);                 /* p. ej. falta dspfirm.cdc */

radio_play(st.url);                      /* http:// o https:// */
/* en el bucle, una vez por fotograma: */
radio_update();
radio_status st;
radio_status_get(&st);                   /* estado, título, kbps, búfer... */
radio_set_volume(0.8f);
radio_stop();                            /* vuelve al instante */

/* al salir: tls_set_abort(true); radio_exit(); ... tls_global_exit() */
```

Cómo funciona ([source/audio/radio3ds.h](source/audio/radio3ds.h)):

- Un **hilo de red** pide el stream con `Icy-MetaData: 1`, separa los
  bloques de metadatos (el título, convertido a UTF-8 si llega en Latin-1) y
  llena un **búfer circular** de 192 KB de MP3 (~12 s a 128 kbps). Si se
  llena, deja de leer y el servidor espera: así se marca el ritmo.
- Un **hilo decodificador** (un punto más prioritario que el principal)
  espera ~2,5 s de audio, decodifica con **minimp3** y encola el PCM en un
  canal de **NDSP** (6 búferes, ~0,6 s). Si el búfer se vacía, vuelve a
  cargar antes de seguir en vez de entrecortarse ("cortes" en pantalla).
- Si la conexión cae, **reconecta** con espera creciente (1, 2, 4, 8 s)
  mientras suena lo que había en el búfer. Tras 6 fallos seguidos sin datos,
  o ante un 4xx o un formato no soportado, se rinde y lo muestra.
  Una emisora que nunca llegó a sonar se reintenta una sola vez: en un
  directorio tan grande, muchas entradas simplemente están caídas.
- **Solo MP3** (`audio/mpeg`). Los streams AAC u Ogg se rechazan con un
  mensaje claro.
- **Seguridad**: la mitad de las emisoras emiten por `http://` y el resto con
  certificados de cualquier CA (o cadenas incompletas). Como el audio es
  público y el `http://` en claro se acepta de todos modos, exigir
  verificación en el `https://` solo dejaría emisoras fuera sin proteger
  nada. Por eso el stream no la exige. Las llamadas a la API, que sí
  importan, siguen verificadas.

## Certificados raíz

La consola no tiene un almacén de certificados utilizable, así que la app
lleva los suyos en `source/net/ca_bundle.c`. Para añadir un servidor:

```sh
# edita tools/hosts.txt y luego:
python tools/gen_ca_bundle.py            # necesita `openssl` y `pip install certifi`
python tools/gen_ca_bundle.py --verify   # comprueba cada host con TLS 1.2
```

El script recorre la cadena real de cada host, busca su raíz en el almacén de
Mozilla (vía `certifi`) y además incluye una lista fija de raíces comunes
(`ALWAYS`: Let's Encrypt, DigiCert, Google, Amazon, Sectigo…) porque los CDN
no siempre encadenan a la misma. Cada raíz suma tiempo de arranque: quita las
que no necesites.

Si un handshake falla con *unknown CA* / `verify 0x00000008`, falta la raíz
de ese host: añádelo a `hosts.txt` y regenera.

Algunos servidores (muchos Icecast autoalojados) **no envían el certificado
intermedio**. Un navegador lo descarga
solo; mbedTLS no. El script lo detecta, descarga el intermedio de la URL
*CA Issuers* del certificado, comprueba con `openssl` que encadena a una raíz
de Mozilla y lo incluye como ancla de confianza, marcado `(intermediate)`.

## Gráficos

`source/gfx/ui.h` envuelve lo básico de citro2d:

```c
ui_begin();
ui_scene(GFX_TOP, COL_BG);
ui_text("Hola", 10, 10, 0.7f, COL_TEXT, 0);
ui_scene(GFX_BOTTOM, COL_BG);
ui_rect(20, 20, 100, 40, COL_ACCENT);
ui_end();
```

El texto se analiza cada fotograma en un búfer que se vacía en `ui_begin()`:
simple y suficiente para unos cientos de glifos. Para textos grandes y
estáticos, analízalos una vez en tu propio `C2D_TextBuf`. Para sprites, usa
`tex3ds` + `C2D_SpriteSheetLoad` desde `romfs:/`.

La demo redibuja a 60 fps. Si tu app es estática, puedes dibujar solo cuando
algo cambie (y cuando `ui_take_restored()` devuelva `true`, al volver del menú
HOME o de la suspensión) y ahorrar batería.

## Licencia

Apache 2.0 (ver `LICENSE` y `NOTICE`).
