# Guía de pruebas — Car Companion en el auto

Esta guía es para cuando tengas el M5 y el Vgate a mano, en el Maxus (o el
MG3). Recorre todas las pantallas construidas hasta ahora y qué esperar en
cada una. Si algo no coincide con lo descrito, esa es la señal de que hay
que capturar el log serial (ver la última sección) y revisarlo.

## Antes de salir

0. **Una sola vez**, para que la sincronización con el backend funcione
   (ver sección 6 más abajo): el archivo `connectivity_secrets.h` ya existe
   (creado con valores de ejemplo para que compile) — hay que editarlo con
   tus datos reales, con un editor de texto (no se los pases al asistente
   por chat). Parado en la **raíz del repo** (`car-companion/`, no adentro
   de `firmware/`):
   ```bash
   open -e firmware/components/connectivity/include/connectivity_secrets.h
   ```
   o si ya estás parado adentro de `firmware/`:
   ```bash
   open -e components/connectivity/include/connectivity_secrets.h
   ```
   Completar `WIFI_SSID`/`WIFI_PASSWORD` (tu WiFi de casa) y
   `BACKEND_EMAIL`/`BACKEND_PASSWORD` (tu cuenta real del backend). Ese
   archivo no se sube a git. Si no lo completás, el dispositivo sigue
   funcionando normal como gauge OBD — solo no va a poder sincronizar
   viajes (confirmado en hardware: con credenciales de ejemplo, intenta
   conectar WiFi, falla, reintenta solo, sin crashear nada).
1. Con el M5 conectado por USB a la Mac, flashear la última versión:
   ```bash
   source ~/esp/esp-idf/export.sh
   cd firmware
   idf.py build
   idf.py -p /dev/cu.usbserial-5B1E0231421 flash
   ```
2. Confirmar que arrancó bien antes de subirte al auto — en la pantalla del
   M5 debería aparecer el dashboard con "SIN OBD" arriba (normal, todavía no
   hay auto encendido cerca).
3. Llevar el Vgate enchufado en el puerto OBD-II del auto (bajo el volante,
   normalmente).

## 1. Dashboard (pantalla principal)

- Al prender el auto (o dar contacto), en unos segundos el estado de arriba
  debería pasar de **"SIN OBD"** a **"OBD OK"** (en verde).
- Con el motor prendido y en ralentí:
  - **RPM**: debería mostrar un número cerca de 700-900 (ralentí típico
    diesel), en **verde**.
  - **Refrigerante**: frío al arrancar (gris/blanco, no es falla), sube con
    el tiempo. Pasado un rato de manejo debería estar en **verde** (90-105°C).
    Si algún día se ve **rojo** con el motor recién prendido, es normal (el
    motor frío parte bajo el umbral verde) — la alarma real es roja
    *después* de que el motor ya estuvo caliente un rato.
  - **Batería**: con el motor prendido, debería estar en **verde** (13.2 a
    14.8V) — eso confirma que el alternador está cargando. Si sale
    **amarillo o rojo** con el motor andando, vale la pena mirarlo en serio
    (correa floja, alternador, etc. — no es un falso positivo del código).
  - **Velocidad**: al manejar, el arco y el número de la izquierda deberían
    seguir la velocidad real (comparar con el velocímetro del auto — no
    tiene que ser exacto al km/h, pero sí cercano).
  - **Boost**: número sin colorear a propósito (no hay referencia real
    todavía). Solo confirmar que cambia al acelerar.
  - **Acelerador**: debería subir a 100% pisando a fondo y bajar a ~0% en
    ralentí.
- **Testigo CHECK**: si el auto tiene la luz de check engine encendida en el
  tablero real, en la pantalla debería aparecer el texto "CHECK" en rojo
  arriba. Si no está encendida en el auto, no debería aparecer nada ahí.
- **Batería del M5** (arriba al centro): baja sola con el uso normal, no
  tiene que ver con el auto. Si baja muy rápido (menos de una hora de uso),
  puede ser el cable USB o el M5 necesitando carga.

## 2. Fallas (DTC)

Menú → **Fallas**.

- **Sin conexión OBD**: si tocás "LEER CODIGOS" *antes* de que el auto esté
  conectado (o con el auto apagado), debería aparecer al toque el mensaje
  "Sin conexion OBD. Conecta el auto y toca de nuevo." — no debería quedarse
  mudo ni trabado en "Leyendo...".
- **Con el auto conectado y sin fallas reales**: al tocar "LEER CODIGOS"
  debería pasar brevemente por "Leyendo..." y terminar en "Sin fallas." en
  verde. Esto ya se probó y funcionó.
- **Con una falla real** (lo que falta probar): si el auto tiene el check
  engine prendido, al leer códigos debería aparecer al menos un código con
  formato `P0301`, `C1201`, etc. (letra + 4 dígitos) en **rojo**, uno por
  línea. Anotar el código que aparece acá — sirve para comparar con lo que
  diga un scanner comercial (o buscar el código en internet) y confirmar que
  el decodificado es correcto.
  - Si en algún momento el auto tiene una falla real conocida (por ejemplo
    porque un mecánico ya la diagnosticó), es la mejor oportunidad para
    validar esta pantalla de punta a punta.
- **Borrar códigos (botón nuevo, 22 ago)**: con al menos un código leído,
  tocar "BORRAR" debería pasar por "Borrando..." y terminar en "Sin fallas."
  Si la falla real sigue activa (no fue algo pasajero), es normal y
  esperable que el código **vuelva a aparecer** la próxima vez que toques
  "LEER" — eso no es un bug del código, es el auto reportando la falla de
  nuevo porque la causa real sigue ahí. Probar también "BORRAR" sin
  conexión OBD: debería mostrar el mismo mensaje "Sin conexion OBD..." que
  "LEER", no quedarse mudo.

## 3. Viaje (historial)

Menú → **Viaje**.

- **Antes del primer viaje real**: debería decir "SIN VIAJES" (ya
  confirmado en el 22 ago).
- **Para generar un viaje real**: hay que manejar con el motor prendido
  (RPM > 0) por **más de 5 minutos seguidos** — viajes más cortos se
  descartan a propósito (no cuentan como "viaje real", ver
  `firmware/README.md`). El viaje se cierra solo cuando:
  - Se apaga el auto (el OBD deja de responder), o
  - El motor queda en ralentí sin moverse por **más de 15 minutos** seguidos
    (pensado para no cortar el viaje si parás a cargar bencina).
- **Después de un viaje real**: al volver a esta pantalla debería mostrar
  automáticamente el viaje más reciente, con:
  - Duración, distancia, combustible usado, velocidad promedio, RPM máximo,
    temperatura máxima de refrigerante, batería mínima, y si hubo check
    engine durante el viaje.
  - Los botones **◀ / ▶** deberían permitir navegar a viajes anteriores (si
    ya hay más de uno guardado).
  - Si el viaje tuvo el check engine encendido en algún momento, el texto
    del cuerpo debería verse en **amarillo** en vez del color normal.
- Cosas a chequear con sentido común: ¿la distancia y duración son
  razonables comparadas con lo que realmente manejaste? No van a ser
  exactas (no hay GPS, la distancia se estima con velocidad), pero no
  deberían estar completamente fuera de rango.
- **Layout rediseñado (23 ago)**: "VOLVER" y los botones **◀ / ▶** ahora
  están abajo (antes arriba, se sentían apretados junto al botón nuevo de
  sync). Arriba a la izquierda queda el texto "VIAJE N/TOTAL", y arriba a
  la derecha un botón solo con el ícono de sincronizar (sin texto). Tocarlo
  le pide a `connectivity` que intente sincronizar ya (en vez de esperar
  hasta 30s del ciclo automático) — no hay feedback visual todavía de si
  funcionó o no, hay que mirar el log serial (sección 6) para confirmarlo.
  Verificar que nada se vea cortado o superpuesto con este layout nuevo.

## 4. Diagnóstico (pantalla nueva, 22 ago)

Menú → **Diagnóstico**.

- Pantalla de solo lectura, se actualiza sola mientras el auto está
  conectado — no tiene botones de acción.
- Debería mostrar:
  - **CONEXION**: "OBD OK" o "SIN OBD", igual que en el dashboard.
  - **ACTUALIZADO**: "hace Xs" — este número debería mantenerse bajo (0-2
    segundos aprox.) mientras el auto está conectado y mandando datos. Si
    empieza a crecer sin parar (10s, 30s, más) con "OBD OK" todavía puesto,
    es señal de que el adaptador se quedó pegado — justo el tipo de cosa
    para la que se hizo esta pantalla.
  - **CARGA MOTOR**, **TEMP ADMISION**, **TEMP AMBIENTE**, **PRESION
    BAROM**, **PRESION RIEL**: son PIDs que no están en el dashboard
    principal. Confirmar que cambian con el uso (ej. carga motor sube al
    acelerar) y que los valores son razonables (temperatura ambiente
    cercana al clima real del día, presión barométrica cercana a 100 kPa a
    nivel del mar).
  - **PRESION RIEL** (common-rail diesel) puede quedar en 0 si el Vgate no
    soporta ese PID en este auto — no es necesariamente un bug, puede ser
    una limitación real del adaptador/auto.

## 5. Mantenimiento (pantalla nueva, rediseñada el 22 ago)

Menú → **Mantenimiento**.

- Dos filas independientes: **ACEITE** y **FILTRO**, cada una con su propio
  contador de km restantes — no siempre se cambian juntos, así que cada uno
  se ajusta y se marca por separado.
- Cada fila tiene 5 botones: **-1K, -100, +100, +1K** (para ajustar los km
  restantes a mano) y **LISTO** (para marcar el cambio hecho y resetear al
  intervalo completo).
- **Primera vez que se calibra un contador real**: como el sistema recién
  empezó a trackear (arranca en 0km propios), usar los botones -1K/-100 para
  bajar "faltan X km" hasta que se acerque a lo que realmente falta según tu
  último cambio real (ej. si sabés que te faltan ~20.000km, tocar "+1K" un
  par de veces, o "-1K" repetido si el valor de partida es mayor). Es un
  ajuste aproximado a mano, no hace falta que quede exacto al km.
- **Con cada viaje real** (>5 min), ambos contadores (aceite y filtro)
  deberían bajar en la misma proporción que la distancia manejada — se
  restan automáticamente, no hace falta tocar nada.
- **Botón "CAMBIO HOY"** (antes "LISTO", rediseñado el 22 ago tras un error
  real de UX en la primera prueba): resetea esa fila (aceite o filtro,
  según cuál se toque) al intervalo completo. Ahora vive en su propia fila,
  separado de los botones de ajuste, en **rojo** en vez de celeste, y pide
  **doble toque**: el primer toque no resetea nada, solo cambia el botón a
  "SEGURO? TOCA" por ~5 segundos — recién el segundo toque en esa ventana
  confirma. Si no se toca de nuevo a tiempo, se desarma solo y vuelve a
  "CAMBIO HOY" sin haber hecho nada. Probar explícitamente: tocar una vez y
  esperar 5s sin tocar de nuevo → nada debería cambiar.
- **Colores**: verde con más de 1000km restantes, amarillo con 1000 o
  menos, rojo con "VENCIDO hace X km" si ya se pasó.
- Los intervalos de partida (10.000 km para ambos) son estimaciones — no
  son el dato del manual del Maxus. Se pueden cambiar en
  `firmware/components/storage/include/storage.h`
  (`STORAGE_OIL_CHANGE_INTERVAL_KM` / `STORAGE_FILTER_CHANGE_INTERVAL_KM`),
  eso solo afecta a qué valor vuelve el contador al tocar "LISTO" — no
  afecta el ajuste manual con los botones +/-.
- **Verificar visualmente que los 5 botones por fila entran bien en la
  pantalla** (320px de ancho) — esto se compiló y flasheó pero no se
  confirmó a ojo en el M5 real todavía, avisar si algún texto se ve cortado
  o los botones se superponen.

## 5.5. Consumo de batería (nuevo, 24 ago)

Con el M5 prendido, sin conectar ni al auto ni al WiFi de casa (el caso que
más importa: la mayor parte del tiempo que el dispositivo pasa prendido), se
midió ~5% de batería gastado en 10 minutos antes de este fix. Dos causas
arregladas: el escaneo BLE quedaba al 100% de duty cycle para siempre, y el
radio WiFi quedaba siempre prendido reintentando conectar. Ahora el WiFi
está **apagado por default** y solo se prende por ventanas cortas (~20s)
cada ~12 minutos si hay algo pendiente de sincronizar, o al tocar el botón
de sync — confirmado en hardware que con 0 viajes pendientes, el radio WiFi
directamente no se prende al arrancar.

**Falta confirmar en el auto real:**
- Repetir la prueba de batería (M5 prendido sin conectar a nada, 10 min,
  ver cuánto bajó) y comparar contra el ~5%/10min original.
- Generar un viaje real (>5min) y confirmar que, pasados los ~12 minutos
  del ciclo automático, el log muestra `prendiendo WiFi para intentar
  sincronizar` y después `sync: N viaje(s) sincronizados` — el ciclo
  automático con radio apagado/prendido no se probó todavía end-to-end.
- Confirmar que el botón de sync manual en Viaje sigue funcionando igual
  que antes (prende el radio al toque, sincroniza, apaga el radio).

Si el consumo sigue siendo alto después de este cambio, avisar — el
siguiente sospechoso (`CONFIG_PM_ENABLE`, light-sleep del CPU) es más
riesgoso y quedó fuera de este cambio a propósito.

## 6. Sincronización con el backend (nuevo, 23 ago)

No es una pantalla — pasa solo en segundo plano. Requiere haber completado
`connectivity_secrets.h` (ver paso 0 de "Antes de salir").

**Confirmado funcionando de punta a punta en hardware real el 24 ago**:
WiFi conecta, la hora sincroniza por SNTP, login + registro + sync de
viajes funcionan — verificado consultando el backend directo con `curl`,
los datos coincidían con lo que mostraba la pantalla de Viaje. Costó cuatro
bugs reales (RAM/TLS + un bug de fechas), todos arreglados — ver
`firmware/README.md` para el detalle.

- **Mientras manejás** (sin WiFi de casa cerca): no debería pasar nada
  visible, ni logs de error repetidos por WiFi — eso es lo esperado, el
  dispositivo sigue funcionando como gauge normal.
- **Al volver a casa**, con el M5 todavía prendido y cerca del WiFi: en el
  log serial (ver sección 8 para cómo capturarlo) buscar:
  - `connectivity: WiFi conectado (IP obtenida), sincronizando hora por SNTP...`
  - `connectivity: hora sincronizada por SNTP: <fecha>` — confirma que el
    reloj del dispositivo es correcto (necesario para el handshake TLS y
    para que la fecha de cada viaje quede bien en el backend).
  - `connectivity: sync: N viaje(s) sincronizados con el backend` — esto
    confirma que el login, el registro del dispositivo, y el POST de
    viajes funcionaron de punta a punta.
  - Si en cambio aparece `connectivity: sync: no se pudo hacer login...`
    revisar el mensaje: si dice "la contraseña debe tener al menos 8
    caracteres", corregir `BACKEND_PASSWORD` en `connectivity_secrets.h`;
    si dice "credenciales inválidas", revisar `BACKEND_EMAIL`/
    `BACKEND_PASSWORD` contra tu cuenta real.
- **Ojo con las fechas**: un viaje que se graba y se sincroniza en el mismo
  arranque del M5 (lo normal de acá en adelante) va a tener la fecha real
  correcta en el backend. Si el M5 se reinicia entre que el viaje pasa y
  que se sincroniza (ej. se queda sin batería, se reflashea), ese viaje
  puntual puede quedar con una fecha aproximada — no es un bug nuevo, es
  una limitación conocida de no tener reloj propio con batería en el M5.
- **`avg_consumption` (nuevo, 24 ago)**: al consultar el historial con
  `curl` (ver más abajo), cada viaje debería traer un `avg_consumption` en
  **L/100km** (litros cada 100km) — comparar contra lo que sabés que gasta
  el Maxus normalmente, para confirmar que el número tiene sentido. Si un
  viaje no trae ese campo (queda ausente, no en `0`), es porque el
  adaptador no expuso el dato de caudal de combustible en ese viaje — no es
  un bug.
- Para confirmar del todo que llegó al backend, se puede pedir el historial
  con `curl` (con tu propio login, no lo hagas pegándome tu contraseña a
  mí):
  ```bash
  TOKEN=$(curl -s -X POST https://car-companion-production.up.railway.app/api/v1/auth/login \
    -H "Content-Type: application/json" \
    -d '{"email":"tu-email","password":"tu-contraseña"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['token'])")
  curl -s https://car-companion-production.up.railway.app/api/v1/sync/trips \
    -H "Authorization: Bearer $TOKEN"
  ```

## 7. Logs nuevos para revisar después de manejar (22 ago)

Estos logs quedan en el M5 mientras maneja — no hace falta estar mirando la
pantalla, se pueden revisar después conectando el M5 a la Mac y usando la
captura serial de la sección 7. Todos con tag `pid_engine`:

- **`OBD desconectado, reintentando...`** — el adaptador se cayó (normal al
  arrancar el motor: el Vgate se resetea con la baja de tensión del
  arranque). Después de esto el dashboard debería volver solo a "SIN OBD" y
  reconectar solo cuando el adaptador vuelva a responder — antes de esta
  fecha, el dashboard se quedaba pegado en "OBD OK" con datos viejos después
  de una desconexión real, sin volver nunca a "SIN OBD". Si ves eso pasar de
  nuevo, es un bug real, avisar.
- **`todavia sin conexion OBD (Xs)`** — aparece cada ~60s mientras no hay
  conexión (ya sea porque nunca conectó, o después de una caída). Si el
  dashboard nunca mostró "OBD OK" en toda la manejada, este log cada 60s
  confirma que el firmware seguía vivo intentando reconectar (no se colgó
  en silencio) — si en cambio el log se corta de golpe y no vuelve a
  aparecer nada, ahí sí hay un problema real (reinicio, crash).
- **`salud: heap_libre=X min_heap_visto=Y obd_conectado=Z uptime=Ns
  stack_libre(pid_engine)=AB stack_libre(nimble_host)=BB`** — cada 5min
  (agregado 24 ago). Si `heap_libre` va bajando de forma sostenida a lo
  largo de varias horas de manejo (no solo un vaivén normal), es un memory
  leak real. Los dos `stack_libre` son nuevos: si alguno se acerca a 0 en
  algún momento (sobre todo `nimble_host`, que hace bastante trabajo por
  callback), es una señal real de que ese stack se quedó corto — avisar,
  porque un stack corto no siempre crashea limpio, puede corromper datos en
  silencio en vez de reiniciar.
- **`todavia sin conexion OBD` / desconexiones seguidas**: si el log
  muestra `WiFi: reintentando conectar en Xs` con `X` creciendo (1, 2, 4,
  8... hasta 30) en vez de reintentar cada vez al toque, es el backoff
  nuevo funcionando bien (24 ago) — antes reintentaba sin pausa todo el
  tiempo que no había WiFi de casa cerca, que es la mayor parte de una
  manejada real.
- **Pantalla de Fallas pegada en "Leyendo..." o "Borrando..."**: no debería
  pasar más (24 ago) — si el comando no se pudo mandar o la respuesta nunca
  llegó, ahora hay un timeout de 5s que la saca sola. Si ves que queda
  pegada más de 5-10s, es un bug real, avisar con el log.

## 8. Si algo no se ve bien

- **Texto raro / caracteres vacíos**: los acentos (á, é, í, ó, ú, ñ) no se
  ven bien con la fuente actual — si aparece un cuadrado vacío en vez de una
  letra, es un bug de tildes, no de datos. Sacar foto y avisar.
- **Pantalla congelada / reinicio solo / pantalla negra**: hay que capturar
  el log serial para ver qué pasó. Con el M5 conectado por USB a la Mac:
  ```bash
  source ~/esp/esp-idf/export.sh
  python3 -c "
  import serial, time
  s = serial.Serial('/dev/cu.usbserial-5B1E0231421', 115200, timeout=1)
  s.dtr = False; s.rts = True; time.sleep(0.1)
  s.rts = False; time.sleep(0.1); s.dtr = True
  end = time.time() + 15
  buf = b''
  while time.time() < end:
      d = s.read(4096)
      if d: buf += d
  print(buf.decode(errors='replace'))
  "
  ```
  Guardar esa salida completa (sobre todo si dice "Guru Meditation",
  "abort()" o "task_wdt") — es lo que hace falta para diagnosticar el
  problema sin estar ahí en el momento.

## Resumen — qué falta validar en el auto

- [ ] Fallas: leer un código DTC **real** (con check engine encendido) y
      confirmar que el código decodificado tiene sentido.
- [ ] Viaje: generar un viaje real de >5 min, confirmar que se guarda y que
      los datos mostrados (duración, distancia, etc.) son razonables.
- [ ] Viaje: confirmar que un viaje se corta bien al apagar el auto y que
      una parada corta (bencina, <15 min) NO corta el viaje en dos.
- [ ] Dashboard: manejando de verdad (no solo detenido), confirmar que
      velocidad y boost muestran datos con carga real.
- [ ] Diagnóstico: confirmar que "ACTUALIZADO" se mantiene bajo con el auto
      conectado, y que los PIDs nuevos (carga motor, temps, presiones)
      tienen sentido.
- [ ] Mantenimiento: confirmar que los botones entran bien en pantalla, que
      ajustar con -1K/-100/+100/+1K calibra bien el contador de aceite y de
      filtro por separado, y que "CAMBIO HOY" pide el doble toque antes de
      resetear (un solo toque no debería cambiar nada).
- [ ] Reconexión real: confirmar que al arrancar el motor (donde el Vgate
      típicamente se resetea por la baja de tensión), el dashboard pasa por
      "SIN OBD" y vuelve solo a "OBD OK" — no debería quedar pegado
      mostrando datos viejos como si siguieran en vivo.
- [x] Sync con el backend: confirmado funcionando de punta a punta en
      hardware real el 24 ago (login, registro, sync de viajes, y fechas
      correctas). Falta seguir viendo que cada viaje real nuevo se
      sincronice bien de acá en adelante, en manejadas normales.
