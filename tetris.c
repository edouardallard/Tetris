#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <fcntl.h>
#include <locale.h>
#include <wchar.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <math.h>

// --- Constants & Config ---
#define BOARD_WIDTH 10
#define BOARD_HEIGHT 20
#define FPS 60
#define FRAME_DELAY_US (1000000 / FPS)

// --- ANSI Colors & Styles ---
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_DIM     "\033[2m"
#define C_REV     "\033[7m"

// Foreground
#define FG_BLACK  "\033[30m"
#define FG_RED    "\033[31m"
#define FG_GREEN  "\033[32m"
#define FG_YELLOW "\033[33m"
#define FG_BLUE   "\033[34m"
#define FG_MAGENTA "\033[35m"
#define FG_CYAN   "\033[36m"
#define FG_WHITE  "\033[37m"
#define FG_GRAY   "\033[90m"

// Background
#define BG_BLACK  "\033[40m"

// --- Symbols ---
#define SYM_BLOCK "██"
#define SYM_SHADOW "░░"
#define SYM_DOT   " ·"

// Borders
#define B_HORZ "══"
#define B_VERT "║"
#define B_TL   "╔"
#define B_TR   "╗"
#define B_BL   "╚"
#define B_BR   "╝"

// --- Game Structures ---
typedef struct {
    int x, y;
} Point;

typedef struct {
    Point blocks[4];
    int color_idx;
    char *color_code;
    int type_idx; // 0-6 for bag logic
} Tetromino;

// --- Globals ---
int board[BOARD_HEIGHT][BOARD_WIDTH] = {0};

// Bag System
int bag[7];
int bag_head = 0;
Tetromino next_queue[3]; // Show 3 next pieces

// Hold System
int hold_idx = -1; // -1 means empty
int hold_locked = 0;

Tetromino current_piece;
int piece_x, piece_y;
int score = 0;
int high_score = 0;
int lines_cleared_total = 0;
int level = 1;
int game_running = 1;
int game_state = 0; // 0 = PLAY, 1 = GAME_OVER
int paused = 0;
struct termios orig_termios;

const char *GAME_OVER_ART[] = {
    " GGG   AAA  M   M EEEE",
    "G     A   A MM MM E   ",
    "G  GG AAAAA M M M EEEE",
    "G   G A   A M   M E   ",
    " GGG  A   A M   M EEEE",
    "",
    " OOO  V   V EEEE RRRR ",
    "O   O V   V E    R   R",
    "O   O V   V EEEE RRRR ",
    "O   O  V V  E    R R  ",
    " OOO    V   EEEE R  RR"
};
const int GAME_OVER_ART_H = 11;
const int GAME_OVER_ART_W = 22;

// --- Persistence ---
void load_high_score() {
    char path[512];
    snprintf(path, sizeof(path), "%s/.tetris_highscore", getenv("HOME"));
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%d", &high_score) != 1) high_score = 0;
        fclose(f);
    }
}

void save_high_score() {
    if (score > high_score) {
        high_score = score;
        char path[512];
        snprintf(path, sizeof(path), "%s/.tetris_highscore", getenv("HOME"));
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "%d", high_score);
            fclose(f);
        }
    }
}

// --- Tetromino Definitions ---
const char* COLORS[] = {
    C_RESET,
    FG_CYAN,    // I
    FG_BLUE,    // J
    FG_YELLOW,  // L 
    FG_WHITE,   // O
    FG_GREEN,   // S
    FG_MAGENTA, // T
    FG_RED      // Z
};

// Definition relative to top-left of 4x4 box
const Tetromino SHAPES[7] = {
    { { {0,1}, {1,1}, {2,1}, {3,1} }, 1, FG_CYAN, 0 },    // I
    { { {0,0}, {0,1}, {1,1}, {2,1} }, 2, FG_BLUE, 1 },    // J
    { { {2,0}, {0,1}, {1,1}, {2,1} }, 3, FG_YELLOW, 2 },  // L
    { { {1,0}, {2,0}, {1,1}, {2,1} }, 4, FG_WHITE, 3 },   // O
    { { {1,0}, {2,0}, {0,1}, {1,1} }, 5, FG_GREEN, 4 },   // S
    { { {1,0}, {0,1}, {1,1}, {2,1} }, 6, FG_MAGENTA, 5 }, // T
    { { {0,0}, {1,0}, {1,1}, {2,1} }, 7, FG_RED, 6 }      // Z
};

// --- Prototypes ---
void init_game();
void reset_game();
void cleanup();
void spawn_piece();
int check_collision(Tetromino p, int x, int y);
void lock_piece();
void rotate_piece();
void drop_piece_hard();
void hold_piece_action();
void handle_input();
void render();
long get_time_ms();
void shuffle_bag();
void fill_next_queue();
Tetromino pop_next_piece();
void load_high_score();
void save_high_score();

// --- Terminal Helper Functions ---
void hide_cursor() { printf("\033[?25l"); }
void show_cursor() { printf("\033[?25h"); }

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(cleanup);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    hide_cursor();
}

void cleanup() {
    save_high_score();
    printf(C_RESET);
    show_cursor();
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[2J\033[H");
}

// --- Game Logic ---

long get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

void shuffle_bag() {
    for (int i = 0; i < 7; i++) bag[i] = i;
    for (int i = 6; i > 0; i--) {
        int j = rand() % (i + 1);
        int temp = bag[i];
        bag[i] = bag[j];
        bag[j] = temp;
    }
    bag_head = 0;
}

Tetromino get_from_bag() {
    if (bag_head >= 7) {
        shuffle_bag();
    }
    return SHAPES[bag[bag_head++]];
}

void fill_next_queue() {
    // Initial fill
    static int first = 1;
    if (first) {
        shuffle_bag();
        for(int i=0; i<3; i++) next_queue[i] = get_from_bag();
        first = 0;
    }
}

Tetromino pop_next_piece() {
    Tetromino p = next_queue[0];
    next_queue[0] = next_queue[1];
    next_queue[1] = next_queue[2];
    next_queue[2] = get_from_bag();
    return p;
}

void reset_game() {
    // Reset board
    for(int y=0; y<BOARD_HEIGHT; y++)
        for(int x=0; x<BOARD_WIDTH; x++)
            board[y][x] = 0;
            
    score = 0;
    lines_cleared_total = 0;
    level = 1;
    game_state = 0; // PLAY
    hold_idx = -1;
    hold_locked = 0;
    
    // Reshuffle queue
    bag_head = 7; // Force reshuffle
    // We need to re-init queue properly
    // Just calling fill_next_queue logic:
    shuffle_bag();
    for(int i=0; i<3; i++) next_queue[i] = get_from_bag();
    
    spawn_piece();
}

void init_game() {
    srand(time(NULL));
    load_high_score();
    enable_raw_mode();
    setlocale(LC_ALL, ""); 
    fill_next_queue();
    spawn_piece();
}

void spawn_piece() {
    current_piece = pop_next_piece();
    piece_x = BOARD_WIDTH / 2 - 2;
    piece_y = 0;
    hold_locked = 0;

    if (check_collision(current_piece, piece_x, piece_y)) {
        game_state = 1; // GAME_OVER
        save_high_score();
    }
}

int check_collision(Tetromino p, int x, int y) {
    for (int i = 0; i < 4; i++) {
        int bx = x + p.blocks[i].x;
        int by = y + p.blocks[i].y;

        if (bx < 0 || bx >= BOARD_WIDTH || by >= BOARD_HEIGHT) return 1;
        if (by >= 0 && board[by][bx]) return 1;
    }
    return 0;
}

void lock_piece() {
    for (int i = 0; i < 4; i++) {
        int bx = piece_x + current_piece.blocks[i].x;
        int by = piece_y + current_piece.blocks[i].y;
        if (by >= 0 && by < BOARD_HEIGHT && bx >= 0 && bx < BOARD_WIDTH) {
            board[by][bx] = current_piece.color_idx;
        }
    }

    int lines = 0;
    for (int y = BOARD_HEIGHT - 1; y >= 0; y--) {
        int full = 1;
        for (int x = 0; x < BOARD_WIDTH; x++) {
            if (board[y][x] == 0) {
                full = 0;
                break;
            }
        }
        if (full) {
            lines++;
            for (int yy = y; yy > 0; yy--) {
                for (int xx = 0; xx < BOARD_WIDTH; xx++) {
                    board[yy][xx] = board[yy - 1][xx];
                }
            }
            for (int xx = 0; xx < BOARD_WIDTH; xx++) board[0][xx] = 0;
            y++; 
        }
    }

    if (lines > 0) {
        lines_cleared_total += lines;
        int points[] = {0, 100, 300, 500, 800};
        score += points[lines] * level;
        level = 1 + (lines_cleared_total / 10);
        if (score > high_score) high_score = score;
    }

    spawn_piece();
}

void hold_piece_action() {
    if (hold_locked) return; 
    
    if (hold_idx == -1) {
        hold_idx = current_piece.type_idx;
        spawn_piece(); // Spawns next from queue
    } else {
        int temp = hold_idx;
        hold_idx = current_piece.type_idx;
        current_piece = SHAPES[temp];
        piece_x = BOARD_WIDTH / 2 - 2;
        piece_y = 0;
    }
    hold_locked = 1;
}

void rotate_piece() {
    Tetromino temp = current_piece;
    if (current_piece.color_idx == 4) return; // O shape

    Point center = temp.blocks[1];
    for (int i = 0; i < 4; i++) {
        int x = temp.blocks[i].x;
        int y = temp.blocks[i].y;
        int rx = x - center.x;
        int ry = y - center.y;
        temp.blocks[i].x = center.x - ry;
        temp.blocks[i].y = center.y + rx;
    }

    if (check_collision(temp, piece_x, piece_y)) {
        if (!check_collision(temp, piece_x - 1, piece_y)) piece_x--;
        else if (!check_collision(temp, piece_x + 1, piece_y)) piece_x++;
        else return; 
    }
    current_piece = temp;
}

void drop_piece_hard() {
    while (!check_collision(current_piece, piece_x, piece_y + 1)) {
        piece_y++;
    }
    lock_piece();
}

int kbhit() {
    struct timeval tv = { 0L, 0L };
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
}

void handle_input() {
    if (!kbhit()) return;

    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return;

    if (c == '\033') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) == 0) return;
        if (read(STDIN_FILENO, &seq[1], 1) == 0) return;
        
        if (seq[0] == '[') {
            if (game_state == 0) {
                switch (seq[1]) {
                    case 'A': rotate_piece(); break; // Up
                    case 'B': if (!check_collision(current_piece, piece_x, piece_y + 1)) piece_y++; break; // Down
                    case 'C': if (!check_collision(current_piece, piece_x + 1, piece_y)) piece_x++; break; // Right
                    case 'D': if (!check_collision(current_piece, piece_x - 1, piece_y)) piece_x--; break; // Left
                }
            }
        }
    } else {
        if (game_state == 0) { // PLAY
            switch(c) {
                case 'q': game_running = 0; break;
                case 'p': paused = !paused; break;
                case ' ': drop_piece_hard(); break;
                case 'c': case 'C': hold_piece_action(); break;
                case 'w': rotate_piece(); break;
                case 'a': if (!check_collision(current_piece, piece_x - 1, piece_y)) piece_x--; break;
                case 's': if (!check_collision(current_piece, piece_x, piece_y + 1)) piece_y++; break;
                case 'd': if (!check_collision(current_piece, piece_x + 1, piece_y)) piece_x++; break;
            }
        } else { // GAME_OVER
            switch(c) {
                case 'q': game_running = 0; break;
                case 'r': reset_game(); break;
            }
        }
    }
    tcflush(STDIN_FILENO, TCIFLUSH);
}

// --- Rendering ---

void render() {
    static int last_w = 0, last_h = 0;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        ws.ws_col = 80;
        ws.ws_row = 24;
    }
    
    int term_w = ws.ws_col;
    int term_h = ws.ws_row;
    
    static char frame_buffer[262144]; 
    char *p = frame_buffer;
    
    if (term_w != last_w || term_h != last_h) {
        p += sprintf(p, "\033[2J");
        last_w = term_w;
        last_h = term_h;
    }
    p += sprintf(p, "\033[H");

    // --- Dynamic Scaling ---
    int panel_width_chars = 26;
    int extra_margin_w = 6; 
    int extra_margin_h = 3; 

    int available_h = term_h - extra_margin_h;
    int available_w_for_board = term_w - panel_width_chars - extra_margin_w;

    int blk_h = available_h / BOARD_HEIGHT;
    if (blk_h < 1) blk_h = 1;
    
    int max_blk_h_by_width = available_w_for_board / (BOARD_WIDTH * 2);
    if (blk_h > max_blk_h_by_width) blk_h = max_blk_h_by_width;
    
    int blk_w = blk_h * 2; 

    int board_pixel_w = BOARD_WIDTH * blk_w;
    int board_pixel_h = BOARD_HEIGHT * blk_h;
    
    int total_content_w = board_pixel_w + 2 + 2 + panel_width_chars + 2;
    int total_content_h = board_pixel_h + 2;

    int margin_top = (term_h - total_content_h) / 2;
    if (margin_top < 0) margin_top = 0;
    int margin_left = (term_w - total_content_w) / 2;
    if (margin_left < 0) margin_left = 0;

    int start_col = margin_left + 1;

    // Ghost Piece
    int ghost_y = piece_y;
    while (!check_collision(current_piece, piece_x, ghost_y + 1)) {
        ghost_y++;
    }

    // -- Draw Top Spacing --
    for (int i = 0; i < margin_top; i++) p += sprintf(p, "\n");

    // -- Top Border --
    p += sprintf(p, "\033[%dG", start_col);
    p += sprintf(p, C_BOLD FG_WHITE B_TL);
    for (int i = 0; i < board_pixel_w / 2; i++) p += sprintf(p, B_HORZ); 
    for (int i = 0; i < board_pixel_w % 2; i++) p += sprintf(p, B_HORZ); 
    p += sprintf(p, B_TR);
    
    p += sprintf(p, "  "); 
    
    p += sprintf(p, B_TL);
    for (int i = 0; i < panel_width_chars - 2; i++) p += sprintf(p, B_HORZ);
    p += sprintf(p, B_TR C_RESET "\n");

    // -- Main Board & Panel Loop --
    for (int y = 0; y < BOARD_HEIGHT; y++) {
        for (int sub_y = 0; sub_y < blk_h; sub_y++) {
            // Move to start column
            p += sprintf(p, "\033[%dG", start_col);
            
            // Board Left Border
            p += sprintf(p, C_BOLD FG_WHITE B_VERT C_RESET);
            
            // Board Row Content
            // Handle GAME OVER Overlay
            int is_overlay_line = 0;
            int overlay_row = -1;
            
            if (game_state == 1) {
                int current_pixel_y = y * blk_h + sub_y;
                
                // We map to art lines. 
                // Just center vertically roughly. 
                // Since blk_h varies, mapping art to block grid is tricky if art is text.
                // Let's just overlay text centered on lines.
                
                int center_y = BOARD_HEIGHT * blk_h / 2;
                int start_art_y = center_y - GAME_OVER_ART_H / 2;
                
                if (current_pixel_y >= start_art_y && current_pixel_y < start_art_y + GAME_OVER_ART_H) {
                    is_overlay_line = 1;
                    overlay_row = current_pixel_y - start_art_y;
                }
            }

            if (is_overlay_line) {
                // Print Art Line Centered
                // Clear board area first? Or draw over.
                // Let's calculate padding.
                const char *art_line = GAME_OVER_ART[overlay_row];
                int art_len = GAME_OVER_ART_W;
                int pad_left = (board_pixel_w - art_len) / 2;
                if (pad_left < 0) pad_left = 0;
                
                // Print background/margin
                for(int i=0; i<pad_left; i++) p += sprintf(p, " ");
                // Print Art
                p += sprintf(p, C_BOLD FG_RED "%s" C_RESET, art_line);
                // Print remaining
                int rem = board_pixel_w - pad_left - art_len;
                for(int i=0; i<rem; i++) p += sprintf(p, " ");
                
            } else {
                for (int x = 0; x < BOARD_WIDTH; x++) {
                    int is_active = 0;
                    int is_ghost = 0;
                    if (game_state == 0) {
                        for (int k = 0; k < 4; k++) {
                            if (current_piece.blocks[k].x + piece_x == x && current_piece.blocks[k].y + piece_y == y) is_active = 1;
                            if (current_piece.blocks[k].x + piece_x == x && current_piece.blocks[k].y + ghost_y == y) is_ghost = 1;
                        }
                    }
                    
                    const char *bg_col = "";
                    
                    if (is_active) {
                         bg_col = current_piece.color_code;
                    } else if (board[y][x] != 0) {
                         bg_col = COLORS[board[y][x]];
                    } else if (is_ghost) {
                         bg_col = C_DIM FG_WHITE;
                    } else {
                         bg_col = C_DIM FG_GRAY;
                    }

                    p += sprintf(p, "%s", bg_col);
                    for(int bw=0; bw < blk_w; bw+=2) {
                        if (is_active || board[y][x] != 0) p += sprintf(p, "██");
                        else if (is_ghost) p += sprintf(p, "░░");
                        else { 
                             if (bw == 0 && (sub_y == blk_h/2 || blk_h == 1)) p += sprintf(p, " ·");
                             else p += sprintf(p, "  ");
                        }
                    }
                    p += sprintf(p, C_RESET);
                }
            }

            // Board Right Border
            p += sprintf(p, C_BOLD FG_WHITE B_VERT C_RESET);

            // Gap
            p += sprintf(p, "  ");

            // -- Side Panel --
            p += sprintf(p, C_BOLD FG_WHITE B_VERT C_RESET " ");
            
            // Robust Side Panel Layout
            int visual_line_idx = y * blk_h + sub_y;
            int total_lines = BOARD_HEIGHT * blk_h;
            
            int content_h = 30;
            int slack = total_lines - content_h;
            if (slack < 0) slack = 0;
            
            int gap = slack / 4; 
            int margin_top_panel = gap;
            
            int y_next = margin_top_panel;
            int y_hold = y_next + 11 + gap;
            int y_stats = y_hold + 6 + gap;
            int y_ctrl = y_stats + 8 + gap; 
            
            char panel_str[256] = "";
            
            if (game_state == 1) {
                 int go_y = total_lines / 2 - 4;
                 // Adjust go_y to avoid conflicting with Art if needed, but side panel is separate.
                 // Just align vaguely center.
                 if (visual_line_idx == go_y) sprintf(panel_str, C_BOLD FG_RED "GAME OVER" C_RESET);
                 else if (visual_line_idx == go_y + 2) sprintf(panel_str, "Final: " FG_YELLOW "%d" C_RESET, score);
                 else if (visual_line_idx == go_y + 3) sprintf(panel_str, "High : " FG_YELLOW "%d" C_RESET, high_score);
                 else if (visual_line_idx == go_y + 5) sprintf(panel_str, "R: Retry");
                 else if (visual_line_idx == go_y + 6) sprintf(panel_str, "Q: Quit");
            } else {
                // NEXT SECTION
                if (visual_line_idx == y_next) sprintf(panel_str, C_BOLD FG_CYAN "NEXT PIECE" C_RESET);
                else if (visual_line_idx >= y_next + 2 && visual_line_idx <= y_next + 10) {
                    int row_rel = visual_line_idx - (y_next + 2);
                    int p_slot = row_rel / 3;
                    int p_row = row_rel % 3;
                    if (p_slot < 3 && p_row < 2) {
                         Tetromino np = next_queue[p_slot];
                         char buf[128] = "    ";
                         for(int px=0; px<4; px++) {
                             int f=0; 
                             for(int k=0; k<4; k++) if(np.blocks[k].x==px && np.blocks[k].y==p_row) f=1;
                             if(f) { strcat(buf, np.color_code); strcat(buf, "██" C_RESET); } else strcat(buf, "  ");
                         }
                         sprintf(panel_str, "%s", buf);
                    }
                }
                
                // HOLD SECTION
                else if (visual_line_idx == y_hold) sprintf(panel_str, C_BOLD FG_MAGENTA "HOLD (C)" C_RESET);
                else if (visual_line_idx >= y_hold + 2 && visual_line_idx <= y_hold + 5) {
                    if (hold_idx != -1) {
                         Tetromino hp = SHAPES[hold_idx];
                         int h_row = visual_line_idx - (y_hold + 2);
                         char buf[128] = "    ";
                         for(int px=0; px<4; px++) {
                             int f=0; 
                             for(int k=0; k<4; k++) if(hp.blocks[k].x==px && hp.blocks[k].y==h_row) f=1;
                             if(f) { strcat(buf, hp.color_code); strcat(buf, "██" C_RESET); } else strcat(buf, "  ");
                         }
                         sprintf(panel_str, "%s", buf);
                    } else {
                        if(visual_line_idx == y_hold + 3) sprintf(panel_str, C_DIM "    Empty" C_RESET);
                    }
                }
                
                // STATS SECTION
                else if (visual_line_idx == y_stats) sprintf(panel_str, "SCORE: " FG_YELLOW "%d" C_RESET, score);
                else if (visual_line_idx == y_stats + 1) sprintf(panel_str, C_DIM "HIGH:  " FG_YELLOW "%d" C_RESET, high_score);
                else if (visual_line_idx == y_stats + 3) sprintf(panel_str, "LEVEL: " FG_GREEN "%d" C_RESET, level);
                else if (visual_line_idx == y_stats + 5) sprintf(panel_str, "LINES: " FG_WHITE "%d" C_RESET, lines_cleared_total);
                
                // CONTROLS SECTION
                else if (visual_line_idx == y_ctrl) sprintf(panel_str, C_DIM "Controls:" C_RESET);
                else if (visual_line_idx == y_ctrl + 1) sprintf(panel_str, C_DIM "Arrows/WASD" C_RESET);
                else if (visual_line_idx == y_ctrl + 2) sprintf(panel_str, C_DIM "Space : Drop" C_RESET);
                else if (visual_line_idx == y_ctrl + 3) sprintf(panel_str, C_DIM "C     : Hold" C_RESET);
                else if (visual_line_idx == y_ctrl + 4) sprintf(panel_str, C_DIM "P     : Pause" C_RESET);
                else if (visual_line_idx == y_ctrl + 6) {
                     if(paused) sprintf(panel_str, C_BOLD FG_RED " PAUSED " C_RESET);
                }
            }

            p += sprintf(p, "%s", panel_str);
            
            int right_border_col = start_col + board_pixel_w + panel_width_chars + 3;
            p += sprintf(p, "\033[%dG", right_border_col); 
            
            p += sprintf(p, " " C_BOLD FG_WHITE B_VERT C_RESET "\n");
        }
    }

    // -- Bottom Border --
    p += sprintf(p, "\033[%dG", start_col);
    p += sprintf(p, C_BOLD FG_WHITE B_BL);
    for (int i = 0; i < board_pixel_w / 2; i++) p += sprintf(p, B_HORZ);
    for (int i = 0; i < board_pixel_w % 2; i++) p += sprintf(p, B_HORZ);
    p += sprintf(p, B_BR);
    
    p += sprintf(p, "  ");
    
    p += sprintf(p, B_BL);
    for (int i = 0; i < panel_width_chars - 2; i++) p += sprintf(p, B_HORZ);
    p += sprintf(p, B_BR C_RESET "\n");

    write(STDOUT_FILENO, frame_buffer, p - frame_buffer);
}

int main(int argc, char *argv[]) {
    int new_window = 0;
    if (argc > 1 && strcmp(argv[1], "--new-window") == 0) new_window = 1;

    if (!new_window && getenv("DISPLAY") != NULL) {
        char path[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (len != -1) {
            path[len] = '\0';
            execlp("gnome-terminal", "gnome-terminal", "--", path, "--new-window", NULL);
            execlp("konsole", "konsole", "-e", path, "--new-window", NULL);
            execlp("xfce4-terminal", "xfce4-terminal", "-x", path, "--new-window", NULL);
            execlp("xterm", "xterm", "-e", path, "--new-window", NULL);
            fprintf(stderr, "Warning: Could not spawn a new terminal window.\n");
        }
    }

    init_game();
    
    long last_drop_time = get_time_ms();
    
    while (game_running) {
        long current_time = get_time_ms();
        
        handle_input();
        
        if (game_state == 0 && !paused) {
            double speed_factor = pow(0.9, (double)(level - 1));
            int drop_interval = (int)(1000.0 * speed_factor);
            if (drop_interval < 50) drop_interval = 50;

            if (current_time - last_drop_time > drop_interval) {
                if (!check_collision(current_piece, piece_x, piece_y + 1)) {
                    piece_y++;
                } else {
                    lock_piece();
                }
                last_drop_time = current_time;
            }
        }

        render();
        
        long render_end = get_time_ms();
        long elapsed = render_end - current_time;
        long sleep_time = (1000 / 60) - elapsed;
        if (sleep_time > 0) usleep(sleep_time * 1000);
    }

    cleanup();
    return 0;
}
