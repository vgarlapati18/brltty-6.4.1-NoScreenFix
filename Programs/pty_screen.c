/*
 * BRLTTY - A background process providing access to the console screen (when in
 *          text mode) for a blind person using a refreshable braille display.
 *
 * Copyright (C) 1995-2022 by The BRLTTY Developers.
 *
 * BRLTTY comes with ABSOLUTELY NO WARRANTY.
 *
 * This is free software, placed under the terms of the
 * GNU Lesser General Public License, as published by the Free Software
 * Foundation; either version 2.1 of the License, or (at your option) any
 * later version. Please see the file LICENSE-LGPL for details.
 *
 * Web Page: http://brltty.app/
 *
 * This software is maintained by Dave Mielke <dave@mielke.cc>.
 */

#include "prologue.h"

#include <string.h>
#include <sys/stat.h>

#include "log.h"
#include "pty_screen.h"
#include "pty_shared.h"

static unsigned int scrollRegionTop;
static unsigned int scrollRegionBottom;

static unsigned char hasColors = 0;
static unsigned char foregroundColor;
static unsigned char backgroundColor;

static unsigned int segmentSize = 0;
static int segmentIdentifier = 0;
static PtyHeader *segmentHeader = NULL;

static unsigned char screenLogLevel = LOG_DEBUG;

void
ptySetScreenLogLevel (unsigned char level) {
  screenLogLevel = level;
}

void
ptyLogSegment (const char *label) {
  logBytes(screenLogLevel, "pty segment: %s", segmentHeader, segmentSize, label);
}

static int
releaseSegment (void) {
  if (shmctl(segmentIdentifier, IPC_RMID, NULL) != -1) return 1;
  logSystemError("shmctl[IPC_RMID]");
  return 0;;
}

static int
allocateSegment (const char *tty) {
  segmentSize = (sizeof(PtyCharacter) * COLS * LINES) + sizeof(PtyHeader);
  key_t key = ptyMakeSegmentKey(tty);

  int found = ptyGetSegmentIdentifier(key, &segmentIdentifier);
  if (found) releaseSegment();

  {
    int flags = IPC_CREAT | S_IRUSR | S_IWUSR;
    found = (segmentIdentifier = shmget(key, segmentSize, flags)) != -1;
  }

  if (found) {
    segmentHeader = ptyAttachSegment(segmentIdentifier);
    if (segmentHeader) return 1;
    releaseSegment();
  } else {
    logSystemError("shmget");
  }

  return 0;
}

static void
saveCursorPosition (void) {
  segmentHeader->cursorRow = getcury(stdscr);
  segmentHeader->cursorColumn = getcurx(stdscr);
}

static PtyCharacter *
getCurrentCharacter (PtyCharacter **end) {
  return ptyGetCharacter(segmentHeader, segmentHeader->cursorRow, segmentHeader->cursorColumn, end);
}

static PtyCharacter *
moveCharacters (PtyCharacter *to, const PtyCharacter *from, unsigned int count) {
  if (count) memmove(to, from, (count * sizeof(*from)));
  return to;
}

static void
setCharacters (PtyCharacter *from, const PtyCharacter *to, const PtyCharacter *character) {
  while (from < to) *from++ = *character;
}

static void
propagateCharacter (PtyCharacter *from, const PtyCharacter *to) {
  setCharacters(from+1, to, from);
}

static void
initializeCharacters (PtyCharacter *from, const PtyCharacter *to) {
  static const PtyCharacter initializer = {
    .text = ' ',
  };

  setCharacters(from, to, &initializer);
}

static void
initializeHeader (void) {
  segmentHeader->headerSize = sizeof(*segmentHeader);
  segmentHeader->segmentSize = segmentSize;

  segmentHeader->characterSize = sizeof(PtyCharacter);
  segmentHeader->charactersOffset = segmentHeader->headerSize;

  segmentHeader->screenHeight = LINES;
  segmentHeader->screenWidth = COLS;
  saveCursorPosition();;

  {
    PtyCharacter *from = ptyGetScreenStart(segmentHeader);
    const PtyCharacter *to = ptyGetScreenEnd(segmentHeader);
    initializeCharacters(from, to);
  }
}

int
ptyBeginScreen (const char *tty) {
  if (initscr()) {
    intrflush(stdscr, FALSE);
    keypad(stdscr, TRUE);

    raw();
    noecho();
    scrollok(stdscr, TRUE);

    scrollRegionTop = getbegy(stdscr);
    scrollRegionBottom = getmaxy(stdscr) - 1;

    hasColors = has_colors();
    foregroundColor = COLOR_WHITE;
    backgroundColor = COLOR_BLACK;

    if (hasColors) {
      start_color();
    }

    if (allocateSegment(tty)) {
      initializeHeader();
      return 1;
    }

    endwin();
  }

  return 0;
}

void
ptyEndScreen (void) {
  endwin();
  ptyDetachSegment(segmentHeader);
  releaseSegment();
}

void
ptyRefreshScreen (void) {
  refresh();
}

void
ptySetCursorPosition (unsigned int row, unsigned int column) {
  move(row, column);
  segmentHeader->cursorRow = row;
  segmentHeader->cursorColumn = column;
}

void
ptySetCursorRow (unsigned int row) {
  ptySetCursorPosition(row, segmentHeader->cursorColumn);
}

void
ptySetCursorColumn (unsigned int column) {
  ptySetCursorPosition(segmentHeader->cursorRow, column);
}

void
ptySetScrollRegion (unsigned int top, unsigned int bottom) {
  scrollRegionTop = top;
  scrollRegionBottom = bottom;
  setscrreg(top, bottom);
}

static void
scrollLines (int amount) {
  scrl(amount);
}

static int
isWithinScrollRegion (unsigned int row) {
  if (row < scrollRegionTop) return 0;
  if (row > scrollRegionBottom) return 0;
  return 1;
}

void
ptyMoveCursorUp (unsigned int amount) {
  unsigned int oldRow = segmentHeader->cursorRow;
  unsigned int newRow = oldRow - amount;

  if (isWithinScrollRegion(oldRow)) {
    int delta = newRow - scrollRegionTop;

    if (delta < 0) {
      scrollLines(delta);
      newRow = scrollRegionTop;
    }
  }

  if (newRow != oldRow) ptySetCursorRow(newRow);
}

void
ptyMoveCursorDown (unsigned int amount) {
  unsigned int oldRow = segmentHeader->cursorRow;
  unsigned int newRow = oldRow + amount;

  if (isWithinScrollRegion(oldRow)) {
    int delta = newRow - scrollRegionBottom;

    if (delta > 0) {
      scrollLines(delta);
      newRow = scrollRegionBottom;
    }
  }

  if (newRow != oldRow) ptySetCursorRow(newRow);
}

void
ptyMoveCursorLeft (unsigned int amount) {
  ptySetCursorColumn(segmentHeader->cursorColumn-amount);
}

void
ptyMoveCursorRight (unsigned int amount) {
  ptySetCursorColumn(segmentHeader->cursorColumn+amount);
}

static PtyCharacter *
setCharacter (unsigned int row, unsigned int column, PtyCharacter **end) {
  cchar_t wch;

  {
    unsigned int oldRow = segmentHeader->cursorRow;
    unsigned int oldColumn = segmentHeader->cursorColumn;
    int move = (row != oldRow) || (column != oldColumn);

    if (move) ptySetCursorPosition(row, column);
    in_wch(&wch);
    if (move) ptySetCursorPosition(oldRow, oldColumn);
  }

  PtyCharacter *character = ptyGetCharacter(segmentHeader, row, column, end);
  character->text = wch.chars[0];
  character->blink = wch.attr & A_BLINK;
  character->bold = wch.attr & A_BOLD;
  character->underline = wch.attr & A_UNDERLINE;
  character->reverse = wch.attr & A_REVERSE;
  character->standout = wch.attr & A_STANDOUT;
  character->dim = wch.attr & A_DIM;

  return character;
}

static PtyCharacter *
setCurrentCharacter (PtyCharacter **end) {
  return setCharacter(segmentHeader->cursorRow, segmentHeader->cursorColumn, end);
}

void
ptyInsertLines (unsigned int count) {
  {
    unsigned int counter = count;
    while (counter-- > 0) insertln();
  }
}

void
ptyDeleteLines (unsigned int count) {
  {
    unsigned int counter = count;
    while (counter-- > 0) deleteln();
  }
}

void
ptyInsertCharacters (unsigned int count) {
  PtyCharacter *end;
  PtyCharacter *from = getCurrentCharacter(&end);

  PtyCharacter *to = from + count;
  if (to > end) to = end;
  moveCharacters(to, from, (end - to));

  {
    unsigned int counter = count;
    while (counter-- > 0) insch(' ');
  }

  setCurrentCharacter(NULL);
  propagateCharacter(from, to);
}

void
ptyDeleteCharacters (unsigned int count) {
  {
    unsigned int counter = count;
    while (counter-- > 0) delch();
  }
}

void
ptyAddCharacter (unsigned char character) {
  unsigned int row = segmentHeader->cursorRow;
  unsigned int column = segmentHeader->cursorColumn;

  addch(character);
  saveCursorPosition();

  setCharacter(row, column, NULL);
}

void
ptySetCursorVisibility (unsigned int visibility) {
  curs_set(visibility);
}

void
ptySetAttributes (attr_t attributes) {
  attrset(attributes);
}

void
ptyAddAttributes (attr_t attributes) {
  attron(attributes);
}

void
ptyRemoveAttributes (attr_t attributes) {
  attroff(attributes);
}

void
ptySetForegroundColor (unsigned char color) {
  foregroundColor = color;
  // FIXME
}

void
ptySetBackgroundColor (unsigned char color) {
  backgroundColor = color;
  // FIXME
}

void
ptyClearToEndOfScreen (void) {
  clrtobot();

  PtyCharacter *from = setCurrentCharacter(NULL);
  const PtyCharacter *to = ptyGetScreenEnd(segmentHeader);
  propagateCharacter(from, to);
}

void
ptyClearToEndOfLine (void) {
  clrtoeol();

  PtyCharacter *to;
  PtyCharacter *from = setCurrentCharacter(&to);
  propagateCharacter(from, to);
}
