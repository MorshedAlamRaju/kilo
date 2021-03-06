/*** includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include<stdio.h>
#include<string.h>
#include<ctype.h>
#include<unistd.h>
#include<termios.h>
#include<stdlib.h>
#include<errno.h>
#include<time.h>
#include<fcntl.h>
#include<stdarg.h>
#include<sys/ioctl.h>
#include<sys/types.h>


/*** defines ***/
#define KILO_VERSION "0.0.1"
#define TAB_STOP 8
#define QUIT_TIMES 1
#define CTRL_KEY(k) ((k) & 0x1f)


enum editorKey{
    BACKSPACE = 127,
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


enum editorHighlight{
    HL_NORMAL = 0,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)


/*** data ***/

struct editorSyntax{
    char *filetype;
    char **filematch;
    int flags;
};


typedef struct erow{
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
} erow;



struct editorConfig{
        int cx, cy; // cx is for col, cy for row
        int rx;
        int rowoff;
        int coloff;
        int screenrows;
        int screencols;
        int numrows;
        int dirty;
        erow *row;
        char *filename;
        char statusmsg[80];
        time_t statusmsg_time;
        struct editorSyntax *syntax;
        struct termios orig_termios; // strcut termios is from termios header
};
struct editorConfig E;



/*** filetypes ***/

char *C_HL_extensions[] = {".c", ".h", ".cpp", NULL};


struct editorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        HL_HIGHLIGHT_NUMBERS
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))




/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));



/*** terminal ***/

void die(const char *s){
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}



void disableRawMode(){
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}


void enableRawMode(){
    if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)die("tcgetattr");
	atexit(disableRawMode);

	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;
	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}


int editorReadKey(){
    int nread;
    char c;
    while((nread = read(STDIN_FILENO, &c, 1)) != 1){
        if(nread == -1 && errno != EAGAIN)
            die("read");
    }

    if(c == '\x1b'){
        char seq[3];

        if(read(STDIN_FILENO, &seq[0], 1) != 1)return '\x1b';
        if(read(STDIN_FILENO, &seq[1], 1) != 1)return '\x1b';

        if(seq[0] == '['){
            if(seq[1] >= '0' && seq[1] <= '9'){
                if(read(STDIN_FILENO, &seq[2], 1) != 1)return '\x1b';
                if(seq[2] == '~'){
                    switch(seq[1]){
                        case '1' : return HOME_KEY;
                        case '3' : return DEL_KEY;
                        case '4' : return END_KEY;
                        case '5' : return PAGE_UP;
                        case '6' : return PAGE_DOWN;
                        case '7' : return HOME_KEY;
                        case '8' : return END_KEY;
                    }
                }
            }
            else{
                switch(seq[1]){
                    case 'A' : return ARROW_UP;
                    case 'B' : return ARROW_DOWN;
                    case 'C' : return ARROW_RIGHT;
                    case 'D' : return ARROW_LEFT;
                    case 'H' : return HOME_KEY;
                    case 'F' : return END_KEY;
                }
            }
        }
        else if(seq[0] == 'O'){
            switch(seq[1]){
                case 'H' : return HOME_KEY;
                case 'F' : return END_KEY;
            }
        }
        return '\x1b';
    }
    else{
        return c;
    }


    return c;
}


int getCursorPosition(int *rows, int *cols){
    char buf[32];
    unsigned int i = 0;

    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4)return -1;

    while(i < sizeof(buf) - 1){
        if(read(STDIN_FILENO, &buf[i], 1) != 1)break;
        if(buf[i] == 'R')break;
        i++;
    }
    buf[i] = '\0';

    if(buf[0] != '\x1b' || buf[1] != '[')return -1;
    if(sscanf(&buf[2], "%d;%d", rows, cols) != 2)return -1;
    return 0;
}


int getWindowSize(int *rows, int *cols){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return getCursorPosition(rows, cols);
    }
    else{
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}



/*** syntax highlighting ***/

int is_separator(char c){
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];",c) != NULL;
}


void editorUpdateSyntax(erow *row){
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    if(E.syntax == NULL)return;
    int prev_sep = 1;

    int i = 0;
    while(i < row->rsize){
        char c = row->render[i];
        unsigned char prev_hl = (i > 0)? row->hl[i - 1] : HL_NORMAL;
        if(E.syntax->flags & HL_HIGHLIGHT_NUMBERS){
            if((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) || (c == '.' && prev_hl == HL_NUMBER)){
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }
        prev_sep = is_separator(c);
        i++;
    }
}


int editorSyntaxToColor(int hl){

    return hl == HL_NUMBER? 31 : hl == HL_MATCH? 34 : 37;
}


void get_file_type(){
    E.syntax = NULL;
    if(E.filename == NULL)return;
    char *ext = strrchr(E.filename, '.');
    unsigned int j;
    for(j = 0; j < HLDB_ENTRIES; ++j){
        struct editorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        while(s->filematch[i]){
            int is_ext = (s->filematch[i][0] == '.');
            if((is_ext && ext && !strcmp(ext, s->filematch[i])) || (!is_ext && strstr(E.filename, s->filematch[i]))){
                E.syntax = s;
                return;
            }
            ++i;
        }
    }

}




/*** row operations ***/

int editorRowCxToRx(erow *row, int cx){
    int rx = 0, j;
    for(j = 0; j < cx; j++){
        if(row->chars[j] == '\t'){
            rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        }
        rx++;
    }
    return rx;
}


int editorRowRxToCx(erow *row, int rx){
    int cur_rx = 0;
    int cx;
    for(cx = 0; cx < row->size; cx++){
        if(row->chars[cx] == '\t'){
            cur_rx += (TAB_STOP - 1) - (cur_rx % TAB_STOP);
        }
        cur_rx++;
        if(cur_rx > rx)return cx;
    }
    return cx;
}


void editorUpdateRow(erow *row){

    int tabs = 0, j, idx = 0;
    for(j = 0; j < row->size; j++){
        if(row->chars[j] == '\t')
            tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);

    for(j = 0; j < row->size; j++){
        if(row->chars[j] == '\t'){
            row->render[idx] = ' ';
            for(idx++; idx % TAB_STOP != 0; idx++)
                row->render[idx] = ' ';
        }
        else{
            row->render[idx] = row->chars[j];
            idx++;
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
    editorUpdateSyntax(row);
}


void editorInsertRow(int at, char *s, size_t len){
    if(at < 0 || at > E.numrows)return;
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(E.row) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;

    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}


void editorFreeRow(erow *row){
    free(row->render);
    free(row->chars);
    free(row->hl);
}


void editorDelRow(int at){
    if(at < 0 || at >= E.numrows)return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at -1));
    E.numrows--;
    E.dirty++;
}


void editorRowInsertChar(erow *row, int at, int c){
    if(at < 0 || at > row->size)at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}


void editorRowAppendString(erow *row, char *s, size_t len){
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}


void editorRowDelChar(erow *row, int at){
    if(at < 0 || at >= row->size)return;
    memmove(&row->chars[at], &row->chars[at+1],row->size-at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}


/*** editor operations ***/

void editorInsertChar(int c){
    if(E.cy == E.numrows){
        editorInsertRow(E.numrows, "",0);
    }
    editorRowInsertChar(&E.row[E.cy],E.cx, c);
    E.cx++;
}


void editorInsertNewline(){
    if(E.cx == 0){
        editorInsertRow(E.cy, "", 0);
    }
    else{
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}


void editorDelChar(){
    if(E.cy == E.numrows)return;
    if(E.cy == 0 && E.cx == 0)return;
    erow *row = &E.row[E.cy];
    if(E.cx > 0){
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    }
    else{
        E.cx = E.row[E.cy-1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}


/*** file i/o ***/


char *editorRowsToString(int *buflen){
    int totlen = 0;
    int j;
    for(j = 0; j < E.numrows; j++){
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for(j = 0; j < E.numrows; j++){
        memcpy(p,E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}


void editorOpen(char *filename){
    free(E.filename);
    E.filename = strdup(filename); // strdup() copies string allocating memory. have to free it.
    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");
//    get_file_type();

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while((linelen = getline(&line, &linecap, fp)) != -1){
            while(linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))linelen--;
            editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}


void editorSave(){
    if(E.filename == NULL){
        E.filename = editorPrompt("Save as: %s (Press ESC to cancel)", NULL);
        if(E.filename == NULL){
            editorSetStatusMessage("Save aborted!!");
            return;
        }
//        get_file_type();
    }

    int len;
    char *buf = editorRowsToString(&len);
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644); // O_creatd new file, if already not created..0644 for permission to ceratd. RDWR for reding and writing.

    if(fd != -1){
        if(ftruncate(fd, len) != -1){ // This function set's the file size to the specified length.
            if(write(fd, buf, len) == len){
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to the disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}


/*** find ***/

void editorFindCallback(char *query, int key){
    static int last_match = -1;
    static int direction = 1;

    static int saved_hl_line;
    static char *saved_hl = NULL;

    if(saved_hl){
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].render);
        free(saved_hl);
        saved_hl = NULL;
    }

    if(key == '\r' || key == '\x1b'){
        last_match = -1;
        direction = 1;
        return;
    }
    else if(key == ARROW_RIGHT || key == ARROW_DOWN){
        direction = 1;
    }
    else if(key == ARROW_UP || key == ARROW_LEFT){
        direction = -1;
    }
    else{
        last_match = -1;
        direction = 1;
    }

    if(last_match == -1)direction = 1;
    int cur = last_match;

    int i;
    for(i = 0; i < E.numrows; ++i){
        cur += direction;
        if(cur == -1)cur = E.numrows - 1;
        else if(cur == E.numrows)cur = 0;

        erow *row = &E.row[cur];
        char *match = strstr(row->render, query);
        if(match){
            last_match = cur;
            E.cy = cur;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowoff = E.numrows;
            saved_hl_line = cur;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl,row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}


void editorFind(){
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;
    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);
    if(query == NULL){
        free(query);
    }
    else{
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }

}


/*** append buffer ***/
struct abuf{
    char *b;
    int len;
};
#define ABUF_INIT {NULL,0}


void abAppend(struct abuf *ab, const char *s, int len){
    char *New = realloc(ab->b, ab->len + len);

    if(New == NULL)return;
    memcpy(&New[ab->len], s, len);
    ab->b = New;
    ab->len += len;
}


void abFree(struct abuf *ab){
    free(ab->b);
}


/*** output ***/

void editorScroll(){
    E.rx = 0;

    if(E.cy < E.numrows){
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if(E.cy < E.rowoff){
        E.rowoff = E.cy;
    }

    if(E.cy >= E.rowoff + E.screenrows){
        E.rowoff = E.cy - E.screenrows + 1;
    }

    if(E.rx < E.coloff){
        E.coloff = E.rx;
    }

    if(E.rx >= E.coloff + E.screencols){
        E.coloff = E.rx - E.screencols + 1;
    }
}



void editorDrawRows(struct abuf *ab){
    int y;
    for(y = 0; y < E.screenrows; y++){
        int filerow = y + E.rowoff;
        if(filerow >= E.numrows){
            if(E.numrows == 0 && y == E.screenrows / 3){
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
                if(welcomelen > E.screencols)welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if(padding){
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while(padding--)abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            }
            else{
                abAppend(ab, "~", 1);
            }
        }
        else{
            int len = E.row[filerow].rsize - E.coloff;
            if(len < 0)len = 0;
            if(len > E.screencols)len = E.screencols;
            char *c = &E.row[filerow].render[E.coloff];
            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int cur_col = -1;
            int j;
            for(j = 0; j < len; ++j){
                if(hl[j] == HL_NORMAL){
                    if(cur_col != -1){
                        abAppend(ab, "\x1b[39m", 5);
                        cur_col = -1;
                    }
                    abAppend(ab, &c[j], 1);
                }
                else{
                    int color = editorSyntaxToColor(hl[j]);
                    if(color != cur_col){
                        cur_col = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab, "\x1b[39m", 5);
        }
        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}


void editorDrawStatusBar(struct abuf *ab){
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%20s - %d lines %s", E.filename? E.filename : "[No Name]", E.numrows, E.dirty? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",E.syntax ? E.syntax->filetype : "filetype unknown", E.cy + 1, E.numrows);
    if(len > E.screencols)len = E.screencols;
    abAppend(ab, status, len);

    while(len < E.screencols){
        if(E.screencols - len == rlen){
            abAppend(ab, rstatus, rlen);
            break;
        }
        else{
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m",3);
    abAppend(ab, "\r\n", 2);
}


void editorDrawMessageBar(struct abuf *ab){
    abAppend(ab, "\x1b[K",3);
    int msglen = strlen(E.statusmsg);
    if(msglen > E.screencols)msglen = E.screencols;
    if(msglen && time(NULL) - E.statusmsg_time < 5){
        abAppend(ab, E.statusmsg, msglen);
    }
}


// 4 means writing 4 bytes in screen, \x1b or 27 is the ESCAPE CHARACTER
// J to clear screen, 2 means whole screen

void editorRefreshScreen(){
    editorScroll();

    struct abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[?251", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);

}


/// va* come from stdarg. vnsprintf comes from stdio. va start, va end deals with variadic functions.
void editorSetStatusMessage(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}


/*** input ***/

char *editorPrompt(char *promt, void (*callback)(char *, int)){
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    size_t  buflen = 0;
    buf[0] = '\0';
    while(1){
        editorSetStatusMessage(promt, buf);
        editorRefreshScreen();
        int c = editorReadKey();
        if(c == DEL_KEY || c == BACKSPACE || c == CTRL_KEY('h')){
            if(buflen != 0)buf[--buflen] = '\0';
        }
        else if(c == '\x1b'){
            editorSetStatusMessage("");
            if(callback)callback(buf, c);
            free(buf);
            return NULL;
        }
        else if(c == '\r'){
            if(buflen != 0){
                editorSetStatusMessage("");
                if(callback)callback(buf, c);
                return buf;
            }
        }
        else if(!iscntrl(c) && c < 128){
            if(buflen == bufsize - 1){
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        if(callback)callback(buf, c);
    }
}


void editorMoveCursor(int key){
    int curLen = (E.cy >= E.numrows) ? 0 : E.row[E.cy].rsize;
    int prevLen = (E.cy > 0) ? E.row[E.cy - 1].rsize : 0;
    int nextLen = (E.cy < E.numrows - 1) ? E.row[E.cy + 1].rsize : 0;
    switch(key){
        case ARROW_LEFT:
            if(E.cx != 0){
                E.cx--;
            }
            else if(E.cy > 0){
                E.cy--;
                E.cx = prevLen;
            }
            break;
        case ARROW_RIGHT:
                if(E.cx < curLen)E.cx++;
                else if(E.cy < E.numrows - 1) {
                    E.cy++;
                    E.cx = 0;
                }
            break;
        case ARROW_UP:
            if(E.cy != 0){
                E.cy--;
                if(E.cx > prevLen)E.cx = prevLen;
            }
            break;
        case ARROW_DOWN:
            if(E.cy < E.numrows - 1){
                E.cy++;
                if(E.cx > nextLen)E.cx = nextLen;
            }
            break;
    }
}


void editorProcessKeypress(){
    static int quit_times = QUIT_TIMES;
    int c = editorReadKey();
    switch(c){
        case '\r':
            editorInsertNewline();
            break;
        case CTRL_KEY('q'):
            if(E.dirty && quit_times > 0){
                editorSetStatusMessage("WARNING!!! File has unchanged changes. Press CTRL - Q again to quit");
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

            break;
        case END_KEY:
            if(E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;


        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if(c == DEL_KEY)editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        case PAGE_DOWN:
        case PAGE_UP:
            {
                if(c == PAGE_UP){
                    E.cy = E.rowoff;
                }
                else if(c == PAGE_DOWN){
                    E.cy = E.rowoff + E.screenrows - 1;
                    if(E.cy > E.numrows)E.cy = E.numrows;
                }
                int times = E.screenrows;
                while(times--){
                    editorMoveCursor(c == PAGE_UP? ARROW_UP : ARROW_DOWN);
                }
            }
            break;
        case ARROW_UP:
        case ARROW_LEFT:
        case ARROW_DOWN:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
                break;


        default:
            editorInsertChar(c);
            break;
    }
    quit_times = QUIT_TIMES;
}


/*** init ***/

void initEditor(){
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.numrows = 0;
    E.row = NULL;
    E.rowoff = 0;
    E.coloff = 0;
    E.dirty = 0;
    E.syntax = NULL;
    E.filename = NULL;

    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;


    if(getWindowSize(&E.screenrows, &E.screencols) == -1)die("getWindowSize");
    E.screenrows -= 2;
}


int main(int argc, char *argv[]){
	enableRawMode();
	initEditor();
	if(argc >= 2){
        editorOpen(argv[1]);
	}

	editorSetStatusMessage("HELP : Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

	while(1){
        editorRefreshScreen();
        editorProcessKeypress();
	}
	return 0;
}






















