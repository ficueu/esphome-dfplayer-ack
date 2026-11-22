# dfplayer_ack — DFPlayer Mini z potwierdzaniem komend

Zewnętrzny komponent ESPHome (≥ 2026.9). Akcje mają te same nazwy co we wbudowanym `dfplayer`, tylko z prefiksem `dfplayer_ack.`. Każda komenda trafia do kolejki, jest wysyłana pojedynczo, a komponent czeka na potwierdzenie. Jak go nie dostanie, wysyła ją jeszcze raz, a jak wszystkie próby zawiodą, zgłasza błąd przez `on_command_failed`.

## Czy DFPlayer w ogóle potwierdza?

Tak. Czwarty bajt ramki (`7E FF 06 CMD **FB** PH PL CS CS EF`) to flaga feedback. Gdy jest ustawiona na `0x01`, moduł odpowiada:

- `41` (ACK): ramka dotarła i została sparsowana,
- `40 xx` (błąd): na przykład `01` busy/inicjalizacja, `04` zła suma kontrolna, `06` brak pliku.

Wbudowany `dfplayer` już wysyła FB=1, ale ACK służy mu tylko do ustawienia flagi `is_playing`. Nie ma tam timeoutu, ponowień ani kolejki: kolejna komenda wysłana zbyt szybko po poprzedniej po prostu przepada.

**ACK ≠ gra.** ACK oznacza tylko, że moduł odebrał ramkę. Że muzyka faktycznie poszła, widać dopiero na pinie **BUSY** (stan LOW w trakcie odtwarzania). Dlatego komponent ma dwa poziomy potwierdzenia:

| poziom | co sprawdza | wymaga |
|---|---|---|
| ACK | moduł odebrał komendę | RX z modułu |
| weryfikacja | BUSY zmienił stan (play → LOW, stop/pause → HIGH) | `busy_pin` (albo `verify_playback: query`) |

## Jak działa

1. **Kolejka FIFO** (`queue_size`): w danej chwili w drodze jest jedna komenda, a między kolejnymi ramkami zachowany jest odstęp `command_interval`.
2. **Czekanie na ACK** przez `ack_timeout`. Brak ACK oznacza ponowienie po `retry_delay`, maksymalnie `max_retries` razy.
3. **Błędy z modułu:**
   - `01` busy, `03` błąd UART, `04` suma kontrolna, `08` odczyt SD → ponowienie (przy busy minimum 500 ms przerwy),
   - `05`/`06` brak utworu, `02`/`09` sleep, `07` advert → natychmiastowy fail, bez ponawiania.
4. **Weryfikacja BUSY** po ACK: play musi zobaczyć LOW, a stop/pause HIGH, w czasie `verify_timeout`. Jeśli się nie uda, komenda idzie jeszcze raz.
5. **Zgubiony ACK, ale BUSY się zmienił** (z HIGH na LOW dla play): komenda jest uznana za wykonaną i nie jest wysyłana ponownie, więc utwór nie zaczyna się od nowa. Działa to także dla `play_next`.
6. **Komendy nieidempotentne** (`play_next`, `play_previous`, `volume_up`, `volume_down`, `play_advert`): bez ACK **nie są ponawiane**, bo moduł mógł je wykonać i zgubić tylko odpowiedź, a wtedy `next` przeskoczyłby dwa utwory. Błąd `0x40` z kodem retriable dalej powoduje ponowienie. Zmienia to `retry_non_idempotent: true`.
7. **Start modułu.** Przez `init_timeout` od startu ESP (oraz po `dfplayer_ack.reset`) timeouty nie zużywają prób. `set_volume` w `on_boot` czeka więc, aż DFPlayer wstanie (ramka `3F`), bez żadnego `delay`.
8. **Samoczynny reset modułu** (niespodziewane `3F`, np. przy spadku napięcia): rośnie `reset_count`, odpala się `on_module_reset`, a ostatnia głośność i EQ są wysyłane ponownie **przed** pozostałymi komendami z kolejki. Po resecie moduł wraca do domyślnej głośności.
9. Kilka `set_volume` z rzędu na końcu kolejki jest scalanych w jedno (wygrywa ostatnia wartość). Kolejność względem innych komend się nie zmienia.
10. Podwójne `3D` (finished), które wysyła wiele modułów, jest odfiltrowywane.

## Konfiguracja

```yaml
external_components:
  - source: github://ficueu/esphome-dfplayer-ack
    components: [dfplayer_ack]

uart:
  id: uart_mp3
  tx_pin: GPIO17
  rx_pin: GPIO18          # wymagany przy use_ack: true
  baud_rate: 9600

dfplayer_ack:
  id: odtwarzacz
  uart_id: uart_mp3
  busy_pin:               # opcjonalny, mocno zalecany
    number: GPIO8
    mode: {input: true, pullup: true}   # bez inverted: komponent sam traktuje LOW jako "gra"
  max_retries: 3          # 0..10, ile razy ponowić (łącznie 1 + max_retries wysłań)
  ack_timeout: 300ms
  retry_delay: 100ms
  command_interval: 50ms
  verify_playback: auto   # auto | busy_pin | query | none  (auto = busy_pin, jeśli podany)
  verify_timeout: 1500ms
  init_timeout: 5s
  use_ack: true           # false dla klonów, które nie odsyłają 0x41 (wtedy tylko BUSY)
  retry_non_idempotent: false
  restore_on_reset: true
  validate_checksum: true # sprawdzanie CS w ramkach z modułu
  queue_size: 16

  retry_count:  {name: "Audio retries"}          # suma ponowień od startu
  failed_count: {name: "Audio failed commands"}  # komendy, które ostatecznie padły
  last_error:   {name: "Audio last error"}       # kod jak niżej
  reset_count:  {name: "Audio module resets"}    # samoczynne resety DFPlayera
  playing:      {name: "Audio playing"}          # z BUSY albo ze stanu komend

  on_finished_playback:   # zmienna: track (uint16, globalny indeks pliku na karcie)
  on_command_failed:      # zmienne: cmd (uint8), error (uint8)
  on_module_reset:
```

Pełny przykład z MQTT dla Node-RED (`<modul>/audio/play` JSON → `play_folder`, błędy na `<modul>/audio/error`) jest w `example.yaml`.

## Akcje

`play_next`, `play_previous`, `play_mp3`, `play` (`file`, `loop`), `play_folder` (`folder`, `file`, `loop`), `play_advert`, `volume_up`, `volume_down`, `set_volume`, `set_eq`, `set_device`, `set_current_track_repeat`, `start`, `pause`, `stop`, `random`, `sleep`, `reset`, `clear_queue`. Warunek: `dfplayer_ack.is_playing`.

Z lambdy dostępne są: `id(x).is_playing()`, `queue_length()`, `get_retry_count()`, `get_failed_count()`, `get_reset_count()`, `get_last_error()` i statyczne `error_to_str(code)`.

## Kody błędów (`error`, `last_error`)

| kod | znaczenie | ponawiane |
|---|---|---|
| 0x01 | moduł zajęty / startuje | tak |
| 0x02 | moduł w trybie sleep | nie |
| 0x03 | błąd odbioru ramki | tak |
| 0x04 | zła suma kontrolna | tak |
| 0x05 | numer poza zakresem | nie |
| 0x06 | brak pliku | nie |
| 0x07 | advert bez grającego utworu | nie |
| 0x08 | błąd odczytu SD | tak |
| 0x09/0x0A | wszedł w sleep | nie |
| 0x80 | brak ACK po wszystkich próbach | — |
| 0x81 | ACK był, ale BUSY/status nie potwierdził odtwarzania | — |
| 0x82 | kolejka pełna, komenda odrzucona | — |

## Ograniczenia, o których trzeba wiedzieć

- **Play w trakcie grania:** BUSY już jest LOW, więc weryfikacja przechodzi od razu, nawet jeśli moduł zignorował komendę. Na tym etapie gwarancję daje tylko ACK. Wiarygodna weryfikacja wymaga startu ze stanu stop.
- **`verify_playback: query`** (bez pinu BUSY) odpytuje status komendą `0x42`. Format odpowiedzi różni się między klonami (YX5200, MH2024K, GD3200B…), więc przed użyciem warto sprawdzić to na swoim module w logu VERBOSE.
- **Klony bez ACK:** jeśli w logu każda komenda kończy się `no ACK`, a muzyka gra, ustaw `use_ack: false` z `busy_pin`. Gdy pojawiają się `RX checksum mismatch`, to `validate_checksum: false`.
- **`track` w `on_finished_playback`** to globalny indeks pliku na karcie (kolejność FAT), a nie folder/plik.
- **Długi brown-out** (dłuższy niż czas wszystkich prób) skończy się `on_command_failed` dla komendy, która była w drodze. Volume/EQ zostaną przywrócone po powrocie modułu, ale samą komendę play musi ponowić Node-RED.

## Strojenie

W logu DEBUG każda udana komenda ma wpis w rodzaju `OK play_folder 2/7 (attempt 1, 210 ms total, ACK after 16 ms)`. Po kilku dniach pracy widać rzeczywiste czasy ACK i startu BUSY na danym module i karcie, i na tej podstawie można ustawić `ack_timeout` oraz `verify_timeout`.

## Testy

Katalog `test/` zawiera emulator DFPlayera na pty (`dfplayer_emu.py`) oraz konfiguracje dla platformy `host`. Emulator celowo gubi ACK, odsyła błędy, opóźnia ACK, psuje sumę kontrolną, nie startuje odtwarzania i symuluje brown-out. Uruchomienie:

```bash
esphome -s port /tmp/dfa_pty compile test/host.yaml
python3 test/dfplayer_emu.py /tmp/dfa_pty test/.esphome/build/dfa-host/.pioenvs/dfa-host/program
```
