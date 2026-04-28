# ETA до конца книги (EPUB) — анализ минимальной реализации

## Короткий вывод
**ДА, реализуемо с минимальными изменениями**: можно считать ETA до конца книги без изменения парсинга EPUB и без тяжелых структур, используя уже существующие `Section::pageCount` из кэша секций и текущий `ReadingEtaTracker`.

## Что уже есть
- ETA-трекер `ReadingEtaTracker` с EMA по времени страницы и API:
  - `updateAndGetMinutes(sectionIndex, pageNumber, remainingPages)`
  - `getAvgMsPerPage()/restoreAvgMsPerPage()`.
- В `EpubReaderActivity::renderStatusBar()` сейчас считается ETA только до конца текущей главы:
  - `remainingPages = section->pageCount - currentPage`.
- Прогресс книги сейчас считается размерно (по cumulative size spine items) через `Epub::calculateProgress(...)`.

## Где обновляется «время чтения страницы»
- `ReadingEtaTracker::updateAndGetMinutes(...)` вызывается в `EpubReaderActivity::renderStatusBar()` на каждом рендере статус-бара.
- Дубликаты рендера той же страницы фильтруются по `pageKey(sectionIndex,pageNumber)`.
- Новая sample (интервал между перелистыванием) добавляется только если интервал в диапазоне 0.8с..10мин.

## Есть ли «общее число страниц книги»
- Явного глобального `totalBookPages` для EPUB в метаданных нет.
- Но есть достаточная база для расчета:
  - у загруженной текущей главы: `section->pageCount`.
  - для любой главы можно быстро получить `pageCount`, загрузив section cache (`Section::loadSectionFile(...)`) без репагинации, если кэш уже создан.
- Текущий код уже умеет прелоадить кэш следующей главы около конца текущей (`prewarmNextSectionCacheIfNeeded`).

## Можно ли посчитать remainingBookPages
Формула:
- `remainingCurrent = section->pageCount - (section->currentPage + 1)`
- `remainingNext = sum(pageCount(spine_i), i = currentSpineIndex+1..last)`
- `remainingBookPages = max(0, remainingCurrent) + remainingNext`

Технически возможно:
- Итерируем по следующим spine index.
- Для каждого создаем временный `Section temp(epub, i, renderer)` и вызываем `loadSectionFile(...)` с текущими reader-настройками.
- Если кэш есть → берем `temp.pageCount`.
- Если кэша нет → **не строим** (чтобы избежать тяжелых операций в статус-баре), просто пропускаем/останавливаем расчёт и не показываем book ETA до прогрева.

## Минимальный способ реализации
1. Добавить в `EpubReaderActivity` helper, например:
   - `std::optional<int> tryCalculateRemainingBookPages() const;`
2. Логика helper:
   - считает остаток текущей главы;
   - суммирует `pageCount` следующих глав, **только если** `loadSectionFile()` успешен;
   - при первом промахе кэша возвращает `std::nullopt` (или partial, но лучше nullopt для честного ETA).
3. В `renderStatusBar()`:
   - вместо `remainingPages` для главы получать `remainingBookPagesOpt`;
   - если есть значение → передать его в `etaTracker.updateAndGetMinutes(currentSpineIndex, section->currentPage, remainingBookPages)`;
   - иначе fallback на текущую логику ETA по главе (или пусто).
4. Кэш/производительность:
   - чтобы не сканировать spine на каждый repaint, кэшировать результат в полях activity (например, `cachedRemainingFuturePages` + `cachedAtSpineIndex/currentPage`) и пересчитывать только при смене страницы/главы.

## Почему это «минимально»
- Без изменений EPUB parser и формат файлов.
- Без новых крупных структур в памяти.
- Переиспользуется текущий `ReadingEtaTracker` (только меняем входной `remainingPages`).
- Изменения локальны в reader activity.

## Ограничения/нюансы
- Если для части следующих глав section cache еще не создан, полный ETA книги может быть временно недоступен.
- При изменении шрифта/интерлинья существующие `pageCount` могут отличаться до полного прогрева кэшей (это уже существующее поведение системы пагинации).

## Псевдокод
```cpp
optional<int> EpubReaderActivity::tryCalculateRemainingBookPages() {
  if (!epub || !section) return nullopt;

  int remaining = max(0, section->pageCount - (section->currentPage + 1));

  const int last = epub->getSpineItemsCount() - 1;
  for (int i = currentSpineIndex + 1; i <= last; ++i) {
    Section s(epub, i, renderer);
    if (!s.loadSectionFile(fontId, lineCompression, extraParagraphSpacing, alignment, hyphenation)) {
      return nullopt; // нет полного знания страниц книги
    }
    remaining += s.pageCount;
  }

  return remaining;
}

void EpubReaderActivity::renderStatusBar() {
  auto remBook = tryCalculateRemainingBookPages();
  int rem = remBook.value_or(max(0, section->pageCount - (section->currentPage + 1)));
  auto eta = etaTracker.updateAndGetMinutes(currentSpineIndex, section->currentPage, rem);
  ...
}
```
