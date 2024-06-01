#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define KILO_VERSION "0.1.0-RC1"
#define KILO_TAB_STOP 8

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

typedef struct erow {
  int size;
  int rsize;
  char *chars;
  char *render;
} erow;

struct editorConfig {
  int cx, cy;
  int rx;
  int rowOffset;
  int columnOffset;
  int screenRows;
  int screenCols;
  int numRows;
  erow *row;
  char *filename;
  char statusMsg[80];
  time_t statusMsgTime;
  struct termios original_termios;
};

struct editorConfig EditorConfig;

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &EditorConfig.original_termios) == -1)
    die("set terminal attribute when disabling raw mode");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &EditorConfig.original_termios) == -1)
    die("get terminal attribute when enabling raw mode");
  atexit(disableRawMode);

  struct termios raw = EditorConfig.original_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("set terminal attribute when enabling raw mode");
}

int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read input");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1':
            case '7':
              return HOME_KEY;
            case '4':
            case '8':
              return END_KEY;
            case '5':
              return PAGE_UP;
            case '6':
              return PAGE_DOWN;
            case '3':
              return DEL_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A':
            return ARROW_UP;
          case 'B':
            return ARROW_DOWN;
          case 'C':
            return ARROW_RIGHT;
          case 'D':
            return ARROW_LEFT;
          case 'H':
            return HOME_KEY;
          case 'F':
            return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
      }
    }

    return '\x1b';
  }

  return c;
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[')
    return -1;

  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    return -1;

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
      return -1;
    return getCursorPosition(rows, cols);
  }
  *rows = ws.ws_row;

  *cols = ws.ws_col;
  return 0;
}

int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
    rx++;
  }

  return rx;
}

void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t')
      tabs++;
  }
  free(row->render);
  row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0)
        row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {
  EditorConfig.row = realloc(EditorConfig.row, sizeof(erow) * (EditorConfig.numRows + 1));

  int at = EditorConfig.numRows;
  EditorConfig.row[at].size = len;
  EditorConfig.row[at].chars = malloc(len + 1);
  memcpy(EditorConfig.row[at].chars, s, len);
  EditorConfig.row[at].chars[len] = '\0';

  EditorConfig.row[at].rsize = 0;
  EditorConfig.row[at].render = NULL;
  editorUpdateRow(&EditorConfig.row[at]);

  EditorConfig.numRows++;
}

void editorOpen(char *filename) {
  free(EditorConfig.filename);
  EditorConfig.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp)
    die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;
    
    editorAppendRow(line, linelen);
  }
  free(line);
  fclose(fp);
}

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL)
    return;

  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

void editorScroll() {
  EditorConfig.rx = 0;
  if (EditorConfig.cy < EditorConfig.numRows) {
    EditorConfig.rx = editorRowCxToRx(&EditorConfig.row[EditorConfig.cy], EditorConfig.cx);
  }

  if (EditorConfig.cy < EditorConfig.rowOffset) {
    EditorConfig.rowOffset = EditorConfig.cy;
  }

  if (EditorConfig.cy >= EditorConfig.rowOffset + EditorConfig.screenRows) {
    EditorConfig.rowOffset = EditorConfig.cy - EditorConfig.screenRows + 1;
  }

  if (EditorConfig.rx < EditorConfig.columnOffset) {
    EditorConfig.columnOffset = EditorConfig.rx;
  }

  if (EditorConfig.rx >= EditorConfig.columnOffset + EditorConfig.screenCols) {
    EditorConfig.columnOffset = EditorConfig.rx - EditorConfig.screenCols + 1;
  }
}

void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < EditorConfig.screenRows; y++) {
    int fileRow = y + EditorConfig.rowOffset;
    if (fileRow >= EditorConfig.numRows) {
      if (EditorConfig.numRows == 0 && y == EditorConfig.screenRows / 3) {
        char welcome[33];
        int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo Editor -- version %s", KILO_VERSION);
        if (welcomelen > EditorConfig.screenCols)
          welcomelen = EditorConfig.screenCols;
        int padding = (EditorConfig.screenCols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = EditorConfig.row[fileRow].rsize - EditorConfig.columnOffset;
      if (len < 0)
        len = 0;
      if (len > EditorConfig.screenCols)
        len = EditorConfig.screenCols;
      abAppend(ab, &EditorConfig.row[fileRow].render[EditorConfig.columnOffset], len);
    }

    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines", EditorConfig.filename ? EditorConfig.filename : "[No Name]", EditorConfig.numRows);
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", EditorConfig.cy + 1, EditorConfig.numRows);
  if (len > EditorConfig.screenCols)
    len = EditorConfig.screenCols;
  abAppend(ab, status, len);
  while (len < EditorConfig.screenCols) {
    if (EditorConfig.screenCols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(EditorConfig.statusMsg);
  if (msglen > EditorConfig.screenCols)
    msglen = EditorConfig.screenCols;
  if (msglen && time(NULL) - EditorConfig.statusMsgTime < 5)
    abAppend(ab, EditorConfig.statusMsg, msglen);
}

void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (EditorConfig.cy - EditorConfig.rowOffset) + 1, (EditorConfig.rx - EditorConfig.columnOffset) + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(EditorConfig.statusMsg, sizeof(EditorConfig.statusMsg), fmt, ap);
  va_end(ap);
  EditorConfig.statusMsgTime = time(NULL);
}

void editorMoveCursor(int key) {
  erow *row = (EditorConfig.cy >= EditorConfig.numRows) ? NULL : &EditorConfig.row[EditorConfig.cy];

  switch (key) {
    case ARROW_LEFT:
      if (EditorConfig.cx != 0) {
        EditorConfig.cx--;
      } else if (EditorConfig.cy > 0) {
        EditorConfig.cy--;
        EditorConfig.cx = EditorConfig.row[EditorConfig.cy].size;
      }
      break;
    case ARROW_DOWN:
      if (EditorConfig.cy < EditorConfig.numRows) {
        EditorConfig.cy++;
      }
      break;
    case ARROW_UP:
      if (EditorConfig.cy != 0) {
        EditorConfig.cy--;
      }
      break;
    case ARROW_RIGHT:
      if (row && EditorConfig.cx < row->size) {
        EditorConfig.cx++;
      } else if (row && EditorConfig.cx == row->size) {
        EditorConfig.cy++;
        EditorConfig.cx = 0;
      }
      break;
  }

  row = (EditorConfig.cy >= EditorConfig.numRows) ? NULL : &EditorConfig.row[EditorConfig.cy];
  int rowLength = row ? row->size : 0;
  if (EditorConfig.cx > rowLength) {
    EditorConfig.cx = rowLength;
  }
}

void editorProcessKeypress() {
  int c = editorReadKey();

  switch (c) {
    case CTRL_KEY('q'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;
    case HOME_KEY:
      EditorConfig.cx = 0;
      break;
    case END_KEY:
      if (EditorConfig.cy < EditorConfig.numRows)
        EditorConfig.cx = EditorConfig.row[EditorConfig.cy].size;
      break;
    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP) {
          EditorConfig.cy = EditorConfig.rowOffset;
        } else if (c == PAGE_DOWN) {
          EditorConfig.cy = EditorConfig.rowOffset + EditorConfig.screenRows - 1;
          if (EditorConfig.cy > EditorConfig.numRows)
            EditorConfig.cy = EditorConfig.numRows;
        }

        int times = EditorConfig.screenRows;
        while (times--) {
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
      }
      break;
    case ARROW_LEFT:
    case ARROW_DOWN:
    case ARROW_UP:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;
  }
}

void initEditor() {
  EditorConfig.cx = 0;
  EditorConfig.cy = 0;
  EditorConfig.rx = 0;
  EditorConfig.rowOffset = 0;
  EditorConfig.columnOffset = 0;
  EditorConfig.numRows = 0;
  EditorConfig.row = NULL;
  EditorConfig.filename = NULL;
  EditorConfig.statusMsg[0] = '\0';
  EditorConfig.statusMsgTime = 0;

  if (getWindowSize(&EditorConfig.screenRows, &EditorConfig.screenCols) == -1)
    die("getWindowSize");

  EditorConfig.screenRows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage("HELP: Ctrl-Q = quit");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}