# Funkcje panelu

Lista funkcji firmware `opengr8on-smartpanel-rp2350` — emulatora modułu Grenton **SMART_PANEL_FM_4**
na płytce Waveshare RP2350-Touch-LCD-4. Szczegóły konfiguracji, budowania i protokołu: [README.md](README.md).

## 1. Przyciski

- **16 przycisków** (`PANEL_BUTTON` 1–16): 4 strony × 4 pola dotykowe (ćwiartki ekranu, łącznie
  z obszarem OLED-a).
- **Tryby przycisku** (`Mode`): monostabilny, bistabilny (przełącza `Value` przy każdym
  naciśnięciu), zablokowany (nic nie zgłasza). Wyjście z trybu bistabilnego zeruje stan.
- **Zdarzenia w CLU**: `OnSwitchOn`, `OnSwitchOff`, `OnChange`, `OnClick`, `OnShortPress`,
  `OnLongPress`, `OnHold` — CLU wylicza je ze zmian `Value`, jak w prawdziwym panelu.
- **Etykieta** (`SetLabel`) w rogu przycisku.
- **Kolorowa ikona** (`SetIconA` / `SetIconB`) pośrodku przycisku — zob. [Ikony](#5-ikony-przycisków).
- **Dioda przy przycisku**: zielona, czerwona, wyłączona; `ShowOK` / `ShowError` powodują miganie
  przez 2 s, po czym dioda wraca do poprzedniego koloru.
- **ButtonsLEDMode** 1: dioda lokalizacyjna widoczna także przy wyłączonej diodzie.
- **Podświetlenie wciśnięcia**: narożnik i poświata w rogu, gasnąca w 260 ms.

## 2. Dotyk i gesty

- **Multi-touch**: do 5 palców naraz, każda ćwiartka to osobny przycisk; palec zostaje przy
  ćwiartce, w której zaczął.
- **Bez przypadkowych naciśnięć**: przeciągnięcie palca (> 24 px) nie jest naciśnięciem;
  naciśnięcie trafia do CLU po 80 ms bez ruchu albo przy puszczeniu (krótkie tapnięcie też
  przechodzi). Środek OLED-a reaguje tylko na gesty.
- **Gesty**: przesunięcie palcem > 80 px w górę / dół / lewo / prawo → `OnGestureUp` / `Down` /
  `Left` / `Right` w CLU. Palec, który zrobił gest, puszcza swój przycisk.
- **GestureDisplayMode**: gest pokazywany na OLED-zie przez 1 s — tekst ikony gestu z CLU albo
  nazwa kierunku.
- **Czułość dotyku** podniesiona względem fabrycznej (lekkie stuknięcia opuszkiem nie giną).

## 3. Strony

- **4 strony** przycisków, wskaźnik strony (kropki) nad OLED-em.
- **Przełączanie gestem** lewo / prawo (lewo = następna), z zawijaniem 4 → 1.
- **Przełączanie z CLU**: `SetNextPage`, `SetPrevPage`, `SetPageNr`.
- **PageControlMode** Command (0): gest nie zmienia strony, działają tylko polecenia z CLU.

## 4. Ekran

- **„OLED” 128×64** pośrodku, skalowany 3×, z wyglądem matrycy punktowej; CLU pisze na nim jak na
  prawdziwym panelu: `PrintText` (także `PrintFloat`), `Draw` / `DisplayContent`, `ClearScreen`.
  Font 5×7, rozmiary 1–3.
- **Komunikat `! TIMEOUT !`**, gdy CLU zacznie rysować i nie wyśle `Draw` w ciągu 1 s.
- **Pasek stanu** pod OLED-em:
  - kropka połączenia: zielona z poświatą = CLU odpytuje, bursztynowa = przydzielony adres, brak
    odpytań, szara = oczekiwanie na CLU,
  - adres RS485 albo — bez adresu — numer seryjny,
  - temperatura.
- **Polskie znaki** z CLU są transliterowane („Światło” → „Swiatlo”), `°` zachowany.
- **Wygładzane fonty** (Noto Sans) dla etykiet i paska stanu.
- **Bez rwania obrazu**: zmiana treści OLED-a nigdy nie rwie obrazu.

## 5. Ikony przycisków

- Pliki **PNG z karty SD** (FAT32 / exFAT) w katalogu `/icons`: ikona `lampa` → `/icons/lampa.png`.
- Prefiks `~` → **negatyw** (odwrócone kolory, przezroczystość bez zmian).
- **IconB** dla przycisku bistabilnego z `Value` = 1, w każdym innym stanie **IconA**.
- **Kolor i przezroczystość**; każdy typ koloru i głębia PNG, bez przeplotu; obraz do 96×96 px,
  wyświetlany w rozmiarze maks. 64×64 (zmniejszany z zachowaniem proporcji).
- Wczytywanie przy pierwszym użyciu (kilka ms), 5 ikon w pamięci naraz.
- Brak karty, pliku albo błąd odczytu: ikona nie jest rysowana, a próba jej wczytania ponawiana
  jest co 30 s.

## 6. Uśpienie ekranu

- **Przyciemnienie** po `ProximityTimeout` (domyślnie 5 s, ustawiane w OM) bez dotyku: w ciągu 1 s
  do 5% jasności, CLU dostaje `OnDisplayOff`; ekran dalej czytelny, przyciski działają.
- **Wygaszenie** 10 s później (podświetlenie i LCD wyłączone, mniejszy pobór prądu); TFBUS działa
  dalej.
- **Budzenie**: dotyk, **stuknięcie w szybkę lub obudowę** albo `SwitchOnDisplay` z CLU →
  `OnDisplayOn`, potem `OnProximityDetect`; od razu pełna jasność, z wygaszenia ~120 ms.
- **Dotyk budzący wygaszony ekran nie trafia do CLU** (nie widać, czego się dotyka).
- **Ochrona LCD przed utrwaleniem obrazu**: przy przyciemnieniu i wygaszeniu obraz przesuwa się
  o 1 px po kwadracie ±2 px (najwyżej co 2 min); nigdy przy pełnej jasności.

## 7. Temperatura i dźwięk

- **Czujnik temperatury** (`PANELSENSTEMP`): odczyt co 1 s, mediana z 5 próbek, wartość wysyłana
  do CLU przy zmianie o 0,1 °C, nie częściej niż co `Sensitivity`.
- **Buzzer**: `Beep` z CLU z częstotliwością (30–5700 Hz), czasem i głośnością (0–16);
  `BuzzerValue` wyłącza dźwięk.

## 8. Współpraca z CLU

- Działa z **prawdziwym CLU Grenton** i z
  [opengr8on-clu-esp32](https://github.com/psobiech/opengr8on-clu-esp32).
- Przedstawia się jako **SMART_PANEL_FM_4**: hwType `0x03`, fwType `3`, fwApiVer `4`.
- **Wykrywanie w OM** (DISCOVER): panel bez adresu zgłasza się sam, w jednej z 32 szczelin
  czasowych, tylko na wolnej magistrali — unikając kolizji z innymi modułami.
- **Gotowa linia do OM.LUA** oraz numer seryjny na ekranie i na USB przy starcie (do ręcznego
  dodania).
- **Numer seryjny** stały dla płytki (z unikalnego ID flash, zakres `0x0f05f000..0x0f05ffff`, z dala
  od prawdziwych paneli) albo ustawiany przy budowaniu.
- **Pełna obsługa adresowania**: RESET, SEEK, ADDR_ASSIGN, STATUS_POLL; VERSION_QUERY bez
  odpowiedzi, tak jak w prawdziwym panelu; każda inna ramka do panelu — ACK.
- **Żadne krótkie dotknięcie nie ginie**: kolejka zmian (24 pozycje), jedna zmiana na odpytanie,
  więc CLU zawsze widzi 1, potem 0.

## 9. Sprzęt

- **Płytka**: Waveshare RP2350-Touch-LCD-4 — RP2350 (240 MHz), LCD 480×480 ST7701 (RGB przez
  PIO + DMA), dotyk GT911 (odczyt 100 Hz), IMU QMI8658, RS485, buzzer (PWM, GPIO 1), gniazdo SD
  (SDIO 4-bit).
- **Obudowa** do druku 3D (`3dprint/rp2350_lcd4_box60.scad`).
- **RS485**: UART1, 115200 8N1, TX GPIO 8, RX GPIO 9; transceiver z automatycznym kierunkiem.
  Podłączenie: A/B → A/B TFBUS, wspólna masa; zalecany pull-up 4,7–10 kΩ z GPIO 8 do 3V3.
- **Bezpieczny start magistrali**: pin TX ustawiany w stan wysoki jako pierwsza operacja w firmware,
  zanim uruchomi się wyświetlacz i dotyk (~1,3 s), aby nie blokować innych modułów.
- **Odpowiedź 800 µs** po zapytaniu — zgodność z ESP32 CLU, który dłużej trzyma DE.
- **Odporność na szum**: złe sumy XOR, ucięte i nieznane ramki są odrzucane; resynchronizacja
  po śmieciach; koniec ramki po 1,5 ms ciszy.
- **Temperatura** z czujnika w QMI8658 (zapasowo: wewnętrzny czujnik RP2350); odrzucane błędy
  I2C i wartości spoza zakresu -30..85 °C; korekta samonagrzewania płytki.
- **Stuknięcie** wykrywane detektorem stuknięć QMI8658 (500 Hz, ±8 g), odczyt co 20 ms; zastępuje
  czujnik zbliżenia prawdziwego panelu.
- **Wygaszenie**: ST7701 Display Off + Sleep In, zatrzymanie PIO + DMA, pętla główna śpi w WFE;
  TFBUS obsługiwany z przerwania UART, GT911 budzi przerwaniem INT.
- **Składanie OLED-a w locie na core1** (`src/scanout.c`): punktów OLED-a nie ma w buforze ramki
  (313 KB zamiast 461 KB); każdy wiersz składany z 1-bitowej treści tuż przed wysłaniem do LCD.
  Przesuwanie obrazu tylko przez tablicę adresów wierszy DMA, bez przerysowywania.
- **Progi dotyku GT911** zapisywane w GT911 tylko przy zmianie (dotyk 60 / puszczenie 40,
  fabrycznie 80 / 60).

## 10. Czego brakuje

- Wgrywanie konfiguracji stron i przycisków z OM — strony mają stały układ; nazwy stron
  i obiektów są pomijane.
- Polecenia rysowania `DrawLine`, `DrawPoint`, `DrawIcon`, `DrawBox`, `ShowButtons`.
- Czujnik zbliżenia i natężenia światła (brak na płytce; światło raportowane jako 0); gesty
  bezdotykowe zastąpione przesunięciami palcem.
- Polskie znaki w fontach (tylko ASCII i `°`).
- Zmiana pliku już wczytanej ikony jest widoczna dopiero po restarcie.

## 11. Diagnostyka (USB)

- Przy starcie: numer seryjny i linia do OM.LUA.
- Co 5 s statystyki RS485: bajty, błędy UART, ramki dobre / złe, odpytania, wysłane ramki, adres,
  ostatnie odebrane bajty.
- Co 30 s surowy odczyt temperatury (`[temp] raw`) — do kalibracji.
- `[panel]` — adres, DISCOVER, zmiany trybu, etykiety, ikony, nieobsłużone ramki.
- `[tap]` — każde stuknięcie; `[icons]` — wczytane ikony i błędy; `[scanout]` — wiersze OLED-a
  złożone za późno; `[loop]` — najdłuższy krok pętli głównej, gdy ≥ 20 ms.

## 12. Rozwój

### Opcje budowania (CMake)

| Opcja | Domyślnie |
|---|---|
| `PANEL_SN` | `0` (z ID flash) |
| `PANEL_DISPLAY_BRIGHTNESS` | `70` % |
| `PANEL_DISPLAY_DIM_BRIGHTNESS` | `5` % |
| `PANEL_DISPLAY_BLANK` | `ON` |
| `PANEL_TAP_WAKE` | `ON` |
| `PANEL_TAP_PEAK_G2` / `PANEL_TAP_QUIET_G2` | `0.5` / `0.2` |
| `PANEL_TOUCH_LEVEL` / `PANEL_TOUCH_LEAVE_LEVEL` | `60` / `40` |
| `PANEL_TEMP_OFFSET` (przez `CMAKE_C_FLAGS`) | `-6.8f` |

### Testy i narzędzia

- **Podgląd UI na PC** (`make -C test/host preview`) — ten sam kod `src/ui.c` co firmware.
- **Testy hosta** z zaślepkami Pico SDK, pod AddressSanitizer/UBSan; pokrycie linii ≥ 95% w CI.
- **Odtwarzanie zrzutu magistrali** (`CAPTURE=…`): ~44 tys. prawdziwych ramek, panel odpowiada
  dokładnie na ramki adresowane do niego.
- Każda wysłana ramka sprawdzana parserem ESP32 CLU (`external/clu-esp32`).
- Logika panelu i protokół (`core/`) w czystym C, bez zależności od sprzętu.
- Generatory: fonty (`tools/gen_font.py`), pliki PNG do testów (`tools/gen_png_fixtures.py`).
