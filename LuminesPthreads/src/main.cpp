#include <pthread.h>
#include <semaphore.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <conio.h>
#include <windows.h>
#else
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

using namespace std;

namespace {

const int BOARD_H = 16;
const int BOARD_W = 10;
const int TARGET_SCORE = 50;
const int START_LIVES = 3;
const int THREAD_COUNT = 8;

struct Cell {
    int color = 0;          // 0 empty, 1 light block, 2 dark block
    bool special = false;   // special blocks expand a connected region
    bool marked = false;    // marked cells are removed by the timeline
    bool specialUsed = false;
};

struct Piece {
    int color[2][2] = {{0, 0}, {0, 0}};
    bool special[2][2] = {{false, false}, {false, false}};
    int row = 0;
    int col = 0;
};

struct StartBarrier {
    pthread_mutex_t mutex{};
    pthread_cond_t cond{};
    int needed = 0;
    int waiting = 0;
    int generation = 0;
};

struct ScoreEntry {
    string name;
    int score = 0;
    string mode;
    string result;
    string when;
};

struct GameState {
    Cell board[BOARD_H][BOARD_W];
    Piece active;
    bool hasActive = false;

    int mode = 1;
    int fallDelayMs = 700;
    int timelineDelayMs = 520;
    int score = 0;
    int lives = START_LIVES;
    int level = 1;
    int timelineCol = -1;
    int comboInSweep = 0;
    int pendingSquares = 0;
    int pendingCells = 0;
    int pendingSpecialCells = 0;
    bool running = true;
    bool paused = false;
    bool gameOver = false;
    bool won = false;
    bool redraw = true;

    deque<char> commands;
    deque<string> events;

    pthread_mutex_t stateMutex{};
    pthread_cond_t redrawCond{};
    StartBarrier barrier;
    sem_t commandSem{};
    sem_t scoreSem{};

    mt19937 rng;
};

void sleepMs(int ms) {
#ifdef _WIN32
    Sleep(static_cast<DWORD>(ms));
#else
    usleep(ms * 1000);
#endif
}

void clearScreen() {
    cout << "\033[2J\033[H";
}

void hideCursor(bool hide) {
    cout << (hide ? "\033[?25l" : "\033[?25h");
}

void enableAnsiIfNeeded() {
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    SetConsoleOutputCP(CP_UTF8);
#endif
}

class TerminalMode {
public:
    TerminalMode() {
#ifndef _WIN32
        if (tcgetattr(STDIN_FILENO, &original_) == 0) {
            configured_ = true;
            termios raw = original_;
            raw.c_lflag &= static_cast<unsigned>(~(ICANON | ECHO));
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
#endif
        hideCursor(true);
    }

    ~TerminalMode() {
#ifndef _WIN32
        if (configured_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &original_);
        }
#endif
        hideCursor(false);
        cout << "\033[0m" << endl;
    }

private:
#ifndef _WIN32
    termios original_{};
    bool configured_ = false;
#endif
};

bool readKey(char &out) {
#ifdef _WIN32
    if (!_kbhit()) {
        return false;
    }
    int c = _getch();
    if (c == 0 || c == 224) {
        int arrow = _getch();
        if (arrow == 75) out = 'a';
        else if (arrow == 77) out = 'd';
        else if (arrow == 72) out = 'w';
        else if (arrow == 80) out = 's';
        else out = 0;
    } else {
        out = static_cast<char>(c);
    }
    return out != 0;
#else
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    timeval timeout{0, 0};
    int ready = select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        return false;
    }
    char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) {
        return false;
    }
    if (c == '\033') {
        char seq[2] = {0, 0};
        if (read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
            if (seq[1] == 'D') out = 'a';
            else if (seq[1] == 'C') out = 'd';
            else if (seq[1] == 'A') out = 'w';
            else if (seq[1] == 'B') out = 's';
            else out = 0;
            return out != 0;
        }
        return false;
    }
    out = c;
    return true;
#endif
}

void barrierInit(StartBarrier &barrier, int needed) {
    pthread_mutex_init(&barrier.mutex, nullptr);
    pthread_cond_init(&barrier.cond, nullptr);
    barrier.needed = needed;
    barrier.waiting = 0;
    barrier.generation = 0;
}

void barrierWait(StartBarrier &barrier) {
    pthread_mutex_lock(&barrier.mutex);
    int generation = barrier.generation;
    barrier.waiting++;
    if (barrier.waiting == barrier.needed) {
        barrier.generation++;
        barrier.waiting = 0;
        pthread_cond_broadcast(&barrier.cond);
    } else {
        while (generation == barrier.generation) {
            pthread_cond_wait(&barrier.cond, &barrier.mutex);
        }
    }
    pthread_mutex_unlock(&barrier.mutex);
}

void barrierDestroy(StartBarrier &barrier) {
    pthread_mutex_destroy(&barrier.mutex);
    pthread_cond_destroy(&barrier.cond);
}

string modeName(int mode) {
    return mode == 1 ? "Lento" : "Rapido";
}

void addEvent(GameState &state, const string &message) {
    state.events.push_front(message);
    while (state.events.size() > 6) {
        state.events.pop_back();
    }
}

int baseColor(const Cell &cell) {
    return cell.color;
}

char blockGlyph(const Cell &cell) {
    if (cell.color == 0) return '.';
    if (cell.marked && cell.special) return '*';
    if (cell.marked) return 'x';
    if (cell.special) return cell.color == 1 ? '@' : '$';
    return cell.color == 1 ? 'O' : '#';
}

bool inside(int row, int col) {
    return row >= 0 && row < BOARD_H && col >= 0 && col < BOARD_W;
}

bool canPlace(const GameState &state, const Piece &piece, int nextRow, int nextCol) {
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 2; ++c) {
            if (piece.color[r][c] == 0) continue;
            int br = nextRow + r;
            int bc = nextCol + c;
            if (!inside(br, bc)) return false;
            if (state.board[br][bc].color != 0) return false;
        }
    }
    return true;
}

Piece randomPiece(GameState &state) {
    uniform_int_distribution<int> colorDist(1, 2);
    uniform_int_distribution<int> specialChance(1, 100);
    uniform_int_distribution<int> specialCell(0, 3);

    Piece piece;
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 2; ++c) {
            piece.color[r][c] = colorDist(state.rng);
            piece.special[r][c] = false;
        }
    }
    if (specialChance(state.rng) <= 18) {
        int idx = specialCell(state.rng);
        piece.special[idx / 2][idx % 2] = true;
    }
    piece.row = 0;
    piece.col = BOARD_W / 2 - 1;
    return piece;
}

void applyBoardGravity(GameState &state) {
    for (int c = 0; c < BOARD_W; ++c) {
        int write = BOARD_H - 1;
        for (int r = BOARD_H - 1; r >= 0; --r) {
            if (state.board[r][c].color != 0) {
                if (write != r) {
                    state.board[write][c] = state.board[r][c];
                    state.board[r][c] = Cell{};
                }
                write--;
            }
        }
        for (int r = write; r >= 0; --r) {
            state.board[r][c] = Cell{};
        }
    }
}

void clearUpperRows(GameState &state) {
    for (int r = 0; r < 4 && r < BOARD_H; ++r) {
        for (int c = 0; c < BOARD_W; ++c) {
            state.board[r][c] = Cell{};
        }
    }
    applyBoardGravity(state);
}

void stopGame(GameState &state, const string &message) {
    addEvent(state, message);
    state.running = false;
    state.gameOver = true;
    state.redraw = true;
    pthread_cond_broadcast(&state.redrawCond);
    sem_post(&state.commandSem);
    sem_post(&state.scoreSem);
}

void loseLife(GameState &state, const string &reason) {
    state.lives--;
    stringstream ss;
    ss << reason << " Vida perdida. Vidas restantes: " << state.lives;
    addEvent(state, ss.str());
    if (state.lives <= 0) {
        state.won = false;
        stopGame(state, "Fin del juego: se agotaron las vidas.");
    } else {
        clearUpperRows(state);
    }
}

void spawnPiece(GameState &state) {
    Piece piece = randomPiece(state);
    if (canPlace(state, piece, piece.row, piece.col)) {
        state.active = piece;
        state.hasActive = true;
        state.redraw = true;
    } else {
        loseLife(state, "La pila alcanzo la zona superior.");
        if (state.running) {
            piece = randomPiece(state);
            if (canPlace(state, piece, piece.row, piece.col)) {
                state.active = piece;
                state.hasActive = true;
            }
        }
    }
}

void landPiece(GameState &state) {
    if (!state.hasActive) return;
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 2; ++c) {
            int br = state.active.row + r;
            int bc = state.active.col + c;
            if (inside(br, bc) && state.active.color[r][c] != 0) {
                state.board[br][bc].color = state.active.color[r][c];
                state.board[br][bc].special = state.active.special[r][c];
                state.board[br][bc].marked = false;
                state.board[br][bc].specialUsed = false;
            }
        }
    }
    state.hasActive = false;
    addEvent(state, "Bloque fijado en el tablero.");

    bool topOccupied = false;
    for (int c = 0; c < BOARD_W; ++c) {
        topOccupied = topOccupied || state.board[0][c].color != 0;
    }
    if (topOccupied) {
        loseLife(state, "Los bloques tocaron la fila superior.");
    }
}

void rotatePiece(GameState &state) {
    if (!state.hasActive) return;
    Piece rotated = state.active;
    rotated.color[0][0] = state.active.color[1][0];
    rotated.color[0][1] = state.active.color[0][0];
    rotated.color[1][0] = state.active.color[1][1];
    rotated.color[1][1] = state.active.color[0][1];

    rotated.special[0][0] = state.active.special[1][0];
    rotated.special[0][1] = state.active.special[0][0];
    rotated.special[1][0] = state.active.special[1][1];
    rotated.special[1][1] = state.active.special[0][1];

    if (canPlace(state, rotated, rotated.row, rotated.col)) {
        state.active = rotated;
        addEvent(state, "Rotacion aplicada.");
    }
}

void hardDrop(GameState &state) {
    if (!state.hasActive) return;
    while (canPlace(state, state.active, state.active.row + 1, state.active.col)) {
        state.active.row++;
    }
    landPiece(state);
}

bool detectSquaresLocked(GameState &state) {
    int newSquares = 0;
    for (int r = 0; r < BOARD_H - 1; ++r) {
        for (int c = 0; c < BOARD_W - 1; ++c) {
            const Cell &a = state.board[r][c];
            const Cell &b = state.board[r][c + 1];
            const Cell &d = state.board[r + 1][c];
            const Cell &e = state.board[r + 1][c + 1];
            if (a.color == 0) continue;
            bool same = baseColor(a) == baseColor(b) &&
                        baseColor(a) == baseColor(d) &&
                        baseColor(a) == baseColor(e);
            if (!same) continue;
            bool fresh = !a.marked || !b.marked || !d.marked || !e.marked;
            if (!fresh) continue;
            state.board[r][c].marked = true;
            state.board[r][c + 1].marked = true;
            state.board[r + 1][c].marked = true;
            state.board[r + 1][c + 1].marked = true;
            newSquares++;
        }
    }

    if (newSquares > 0) {
        state.pendingSquares += newSquares;
        stringstream ss;
        ss << "Combo detectado: " << newSquares << " cuadrado(s) 2x2.";
        addEvent(state, ss.str());
        state.redraw = true;
        sem_post(&state.scoreSem);
        pthread_cond_signal(&state.redrawCond);
        return true;
    }
    return false;
}

int expandSpecialLocked(GameState &state) {
    int totalNewMarks = 0;
    const int dr[4] = {-1, 1, 0, 0};
    const int dc[4] = {0, 0, -1, 1};

    for (int sr = 0; sr < BOARD_H; ++sr) {
        for (int sc = 0; sc < BOARD_W; ++sc) {
            Cell &start = state.board[sr][sc];
            if (start.color == 0 || !start.special || !start.marked || start.specialUsed) {
                continue;
            }
            start.specialUsed = true;
            int target = start.color;
            vector<pair<int, int>> stack;
            stack.push_back({sr, sc});
            vector<vector<bool>> seen(BOARD_H, vector<bool>(BOARD_W, false));
            seen[sr][sc] = true;

            while (!stack.empty()) {
                auto [r, c] = stack.back();
                stack.pop_back();
                for (int i = 0; i < 4; ++i) {
                    int nr = r + dr[i];
                    int nc = c + dc[i];
                    if (!inside(nr, nc) || seen[nr][nc]) continue;
                    seen[nr][nc] = true;
                    Cell &next = state.board[nr][nc];
                    if (next.color != target) continue;
                    if (!next.marked) {
                        next.marked = true;
                        totalNewMarks++;
                    }
                    stack.push_back({nr, nc});
                }
            }
        }
    }

    if (totalNewMarks > 0) {
        state.pendingSpecialCells += totalNewMarks;
        stringstream ss;
        ss << "Bloque especial activo: " << totalNewMarks << " celda(s) conectadas marcadas.";
        addEvent(state, ss.str());
        state.redraw = true;
        sem_post(&state.scoreSem);
        pthread_cond_signal(&state.redrawCond);
    }
    return totalNewMarks;
}

int clearTimelineColumnLocked(GameState &state, int col) {
    if (col < 0 || col >= BOARD_W) return 0;
    int cleared = 0;
    for (int r = 0; r < BOARD_H; ++r) {
        if (state.board[r][col].marked) {
            state.board[r][col] = Cell{};
            cleared++;
        }
    }
    if (cleared > 0) {
        state.pendingCells += cleared;
        applyBoardGravity(state);
        stringstream ss;
        ss << "Linea de tiempo limpio columna " << col << " (" << cleared << " celda(s)).";
        addEvent(state, ss.str());
        state.redraw = true;
        sem_post(&state.scoreSem);
        pthread_cond_signal(&state.redrawCond);
    }
    return cleared;
}

void updateDifficultyLocked(GameState &state) {
    int newLevel = 1 + state.score / 25;
    if (newLevel > state.level) {
        state.level = newLevel;
        state.fallDelayMs = max(230, state.fallDelayMs - 80);
        state.timelineDelayMs = max(170, state.timelineDelayMs - 50);
        stringstream ss;
        ss << "Nivel " << state.level << ": velocidad incrementada.";
        addEvent(state, ss.str());
    }
}

void processScoreLocked(GameState &state) {
    int squares = state.pendingSquares;
    int cells = state.pendingCells;
    int specialCells = state.pendingSpecialCells;
    state.pendingSquares = 0;
    state.pendingCells = 0;
    state.pendingSpecialCells = 0;

    if (squares == 0 && cells == 0 && specialCells == 0) return;

    int chainBonus = 0;
    if (squares > 0) {
        int before = state.comboInSweep;
        state.comboInSweep += squares;
        chainBonus = before * 5 + max(0, squares - 1) * 5;
    }

    int gained = squares * 10 + cells + specialCells * 2 + chainBonus;
    state.score += gained;
    stringstream ss;
    ss << "+" << gained << " puntos";
    if (squares > 0) ss << " (" << squares << " cuadrado(s)";
    if (chainBonus > 0) ss << ", bono cadena " << chainBonus;
    if (squares > 0) ss << ")";
    addEvent(state, ss.str());
    updateDifficultyLocked(state);

    if (state.score >= TARGET_SCORE && state.running) {
        state.won = true;
        stopGame(state, "Victoria: meta de 50 puntos alcanzada.");
    }
}

void *inputThread(void *arg) {
    GameState *state = static_cast<GameState *>(arg);
    barrierWait(state->barrier);
    while (true) {
        pthread_mutex_lock(&state->stateMutex);
        bool running = state->running;
        pthread_mutex_unlock(&state->stateMutex);
        if (!running) break;

        char key = 0;
        if (readKey(key)) {
            key = static_cast<char>(tolower(key));
            pthread_mutex_lock(&state->stateMutex);
            state->commands.push_back(key);
            pthread_mutex_unlock(&state->stateMutex);
            sem_post(&state->commandSem);
        }
        sleepMs(18);
    }
    return nullptr;
}

void *playerThread(void *arg) {
    GameState *state = static_cast<GameState *>(arg);
    barrierWait(state->barrier);
    while (true) {
        sem_wait(&state->commandSem);
        pthread_mutex_lock(&state->stateMutex);
        if (!state->running && state->commands.empty()) {
            pthread_mutex_unlock(&state->stateMutex);
            break;
        }
        if (state->commands.empty()) {
            pthread_mutex_unlock(&state->stateMutex);
            continue;
        }
        char key = state->commands.front();
        state->commands.pop_front();

        if (key == 'q') {
            state->won = false;
            stopGame(*state, "Partida cancelada por el jugador.");
        } else if (key == 'p') {
            state->paused = !state->paused;
            addEvent(*state, state->paused ? "Pausa activada." : "Pausa desactivada.");
        } else if (!state->paused && state->hasActive) {
            if (key == 'a' && canPlace(*state, state->active, state->active.row, state->active.col - 1)) {
                state->active.col--;
            } else if (key == 'd' && canPlace(*state, state->active, state->active.row, state->active.col + 1)) {
                state->active.col++;
            } else if (key == 's') {
                if (canPlace(*state, state->active, state->active.row + 1, state->active.col)) {
                    state->active.row++;
                } else {
                    landPiece(*state);
                }
            } else if (key == 'w' || key == 'r') {
                rotatePiece(*state);
            } else if (key == ' ') {
                hardDrop(*state);
            }
        }
        state->redraw = true;
        pthread_cond_signal(&state->redrawCond);
        pthread_mutex_unlock(&state->stateMutex);
    }
    return nullptr;
}

void *gravityThread(void *arg) {
    GameState *state = static_cast<GameState *>(arg);
    barrierWait(state->barrier);
    while (true) {
        pthread_mutex_lock(&state->stateMutex);
        bool running = state->running;
        int delay = state->fallDelayMs;
        pthread_mutex_unlock(&state->stateMutex);
        if (!running) break;

        sleepMs(delay);
        pthread_mutex_lock(&state->stateMutex);
        if (state->running && !state->paused) {
            if (!state->hasActive) {
                spawnPiece(*state);
            } else if (canPlace(*state, state->active, state->active.row + 1, state->active.col)) {
                state->active.row++;
            } else {
                landPiece(*state);
            }
            state->redraw = true;
            pthread_cond_signal(&state->redrawCond);
        }
        pthread_mutex_unlock(&state->stateMutex);
    }
    return nullptr;
}

void *timelineThread(void *arg) {
    GameState *state = static_cast<GameState *>(arg);
    barrierWait(state->barrier);
    while (true) {
        pthread_mutex_lock(&state->stateMutex);
        bool running = state->running;
        int delay = state->timelineDelayMs;
        pthread_mutex_unlock(&state->stateMutex);
        if (!running) break;

        sleepMs(delay);
        pthread_mutex_lock(&state->stateMutex);
        if (state->running && !state->paused) {
            state->timelineCol++;
            if (state->timelineCol >= BOARD_W) {
                state->timelineCol = 0;
                state->comboInSweep = 0;
                addEvent(*state, "Nuevo barrido de la linea de tiempo.");
            }
            clearTimelineColumnLocked(*state, state->timelineCol);
            state->redraw = true;
            pthread_cond_signal(&state->redrawCond);
        }
        pthread_mutex_unlock(&state->stateMutex);
    }
    return nullptr;
}

void *detectorThread(void *arg) {
    GameState *state = static_cast<GameState *>(arg);
    barrierWait(state->barrier);
    while (true) {
        sleepMs(140);
        pthread_mutex_lock(&state->stateMutex);
        if (!state->running) {
            pthread_mutex_unlock(&state->stateMutex);
            break;
        }
        if (!state->paused) {
            detectSquaresLocked(*state);
        }
        pthread_mutex_unlock(&state->stateMutex);
    }
    return nullptr;
}

void *specialThread(void *arg) {
    GameState *state = static_cast<GameState *>(arg);
    barrierWait(state->barrier);
    while (true) {
        sleepMs(170);
        pthread_mutex_lock(&state->stateMutex);
        if (!state->running) {
            pthread_mutex_unlock(&state->stateMutex);
            break;
        }
        if (!state->paused) {
            expandSpecialLocked(*state);
        }
        pthread_mutex_unlock(&state->stateMutex);
    }
    return nullptr;
}

void *scoreThread(void *arg) {
    GameState *state = static_cast<GameState *>(arg);
    barrierWait(state->barrier);
    while (true) {
        sem_wait(&state->scoreSem);
        pthread_mutex_lock(&state->stateMutex);
        bool shouldExit = !state->running &&
                          state->pendingSquares == 0 &&
                          state->pendingCells == 0 &&
                          state->pendingSpecialCells == 0;
        if (shouldExit) {
            pthread_mutex_unlock(&state->stateMutex);
            break;
        }
        processScoreLocked(*state);
        state->redraw = true;
        pthread_cond_signal(&state->redrawCond);
        pthread_mutex_unlock(&state->stateMutex);
    }
    return nullptr;
}

void drawGameLocked(const GameState &state) {
    vector<vector<Cell>> view(BOARD_H, vector<Cell>(BOARD_W));
    for (int r = 0; r < BOARD_H; ++r) {
        for (int c = 0; c < BOARD_W; ++c) {
            view[r][c] = state.board[r][c];
        }
    }
    if (state.hasActive) {
        for (int r = 0; r < 2; ++r) {
            for (int c = 0; c < 2; ++c) {
                int br = state.active.row + r;
                int bc = state.active.col + c;
                if (inside(br, bc)) {
                    view[br][bc].color = state.active.color[r][c];
                    view[br][bc].special = state.active.special[r][c];
                    view[br][bc].marked = false;
                }
            }
        }
    }

    stringstream out;
    out << "\033[H";
    out << "+---------------- LUMINES PTHREADS ----------------+\n";
    out << "Modo: " << left << setw(7) << modeName(state.mode)
        << "  Puntaje: " << setw(3) << state.score << "/" << TARGET_SCORE
        << "  Vidas: " << state.lives
        << "  Nivel: " << state.level
        << "  Linea: " << setw(2) << state.timelineCol
        << (state.paused ? "  [PAUSA]" : "         ") << "\n";
    out << "Controles: A/D mover | W rotar | S bajar | ESP soltar | P pausa | Q salir\n\n";
    out << "     ";
    for (int c = 0; c < BOARD_W; ++c) {
        out << (c == state.timelineCol ? "vv" : "  ");
    }
    out << "\n";
    out << "   +" << string(BOARD_W * 2, '-') << "+\n";
    for (int r = 0; r < BOARD_H; ++r) {
        out << setw(2) << r << " |";
        for (int c = 0; c < BOARD_W; ++c) {
            char g = blockGlyph(view[r][c]);
            out << g << g;
        }
        out << "|\n";
    }
    out << "   +" << string(BOARD_W * 2, '-') << "+\n";
    out << "Leyenda: OO color claro, ## color oscuro, @@/$$ especiales, xx marcado.\n";
    out << "Eventos recientes:\n";
    for (const string &event : state.events) {
        out << " - " << event << "\n";
    }
    out << string(70, ' ') << "\n";
    cout << out.str();
    cout.flush();
}

void *rendererThread(void *arg) {
    GameState *state = static_cast<GameState *>(arg);
    barrierWait(state->barrier);
    clearScreen();
    while (true) {
        pthread_mutex_lock(&state->stateMutex);
        bool running = state->running;
        if (state->redraw || !running) {
            drawGameLocked(*state);
            state->redraw = false;
        }
        pthread_mutex_unlock(&state->stateMutex);
        if (!running) break;
        sleepMs(70);
    }
    return nullptr;
}

void initGame(GameState &state, int mode) {
    state = GameState{};
    state.mode = mode;
    state.fallDelayMs = mode == 1 ? 720 : 460;
    state.timelineDelayMs = mode == 1 ? 540 : 310;
    state.rng.seed(static_cast<unsigned>(chrono::system_clock::now().time_since_epoch().count()));
    pthread_mutex_init(&state.stateMutex, nullptr);
    pthread_cond_init(&state.redrawCond, nullptr);
    barrierInit(state.barrier, THREAD_COUNT);
    sem_init(&state.commandSem, 0, 0);
    sem_init(&state.scoreSem, 0, 0);
    addEvent(state, "Partida iniciada en modo " + modeName(mode) + ".");
}

void destroyGame(GameState &state) {
    sem_destroy(&state.commandSem);
    sem_destroy(&state.scoreSem);
    barrierDestroy(state.barrier);
    pthread_cond_destroy(&state.redrawCond);
    pthread_mutex_destroy(&state.stateMutex);
}

vector<ScoreEntry> loadScores() {
    vector<ScoreEntry> scores;
    ifstream in("high_scores.txt");
    string line;
    while (getline(in, line)) {
        stringstream ss(line);
        ScoreEntry e;
        string scoreText;
        getline(ss, e.name, '|');
        getline(ss, scoreText, '|');
        getline(ss, e.mode, '|');
        getline(ss, e.result, '|');
        getline(ss, e.when, '|');
        if (!e.name.empty()) {
            e.score = atoi(scoreText.c_str());
            scores.push_back(e);
        }
    }
    sort(scores.begin(), scores.end(), [](const ScoreEntry &a, const ScoreEntry &b) {
        return a.score > b.score;
    });
    if (scores.size() > 10) scores.resize(10);
    return scores;
}

string nowText() {
    time_t now = time(nullptr);
    tm *local = localtime(&now);
    stringstream ss;
    ss << put_time(local, "%Y-%m-%d %H:%M");
    return ss.str();
}

void saveScore(const string &name, int score, const string &mode, const string &result) {
    vector<ScoreEntry> scores = loadScores();
    scores.push_back({name, score, mode, result, nowText()});
    sort(scores.begin(), scores.end(), [](const ScoreEntry &a, const ScoreEntry &b) {
        return a.score > b.score;
    });
    if (scores.size() > 10) scores.resize(10);
    ofstream out("high_scores.txt", ios::trunc);
    for (const ScoreEntry &e : scores) {
        out << e.name << '|' << e.score << '|' << e.mode << '|' << e.result << '|' << e.when << '\n';
    }
}

void showScores() {
    clearScreen();
    cout << "PUNTAJES DESTACADOS\n";
    cout << "-------------------\n";
    vector<ScoreEntry> scores = loadScores();
    if (scores.empty()) {
        cout << "Aun no hay puntajes guardados.\n";
    } else {
        cout << left << setw(4) << "#" << setw(18) << "Jugador" << setw(10) << "Puntos"
             << setw(10) << "Modo" << setw(12) << "Resultado" << "Fecha\n";
        for (size_t i = 0; i < scores.size(); ++i) {
            cout << left << setw(4) << (i + 1)
                 << setw(18) << scores[i].name.substr(0, 17)
                 << setw(10) << scores[i].score
                 << setw(10) << scores[i].mode
                 << setw(12) << scores[i].result
                 << scores[i].when << "\n";
        }
    }
    cout << "\nPresiona ENTER para volver al menu.";
    string pause;
    getline(cin, pause);
}

void showInstructions() {
    clearScreen();
    cout << "INSTRUCCIONES\n";
    cout << "-------------\n";
    cout << "Objetivo: formar cuadrados 2x2 del mismo color y llegar a 50 puntos.\n";
    cout << "Pierdes una vida si la pila alcanza la fila superior. Tienes 3 vidas.\n\n";
    cout << "Controles dentro de la partida:\n";
    cout << "  A / Flecha izquierda  : mover bloque a la izquierda\n";
    cout << "  D / Flecha derecha    : mover bloque a la derecha\n";
    cout << "  W / Flecha arriba     : rotar bloque 2x2\n";
    cout << "  S / Flecha abajo      : bajar una fila\n";
    cout << "  ESPACIO               : soltar inmediatamente\n";
    cout << "  P                     : pausar o continuar\n";
    cout << "  Q                     : terminar la partida\n\n";
    cout << "Mecanica Lumines implementada:\n";
    cout << "  - El bloque activo cae de forma automatica.\n";
    cout << "  - La linea de tiempo barre columnas y elimina celdas marcadas.\n";
    cout << "  - Los cuadrados 2x2 del mismo color se marcan como combo.\n";
    cout << "  - Los bloques especiales (@@ y $$) marcan regiones conectadas.\n";
    cout << "  - Varias combinaciones en el mismo barrido dan bono de cadena.\n\n";
    cout << "Presiona ENTER para volver al menu.";
    string pause;
    getline(cin, pause);
}

void printMenu() {
    clearScreen();
    cout << "+--------------------------------------------------+\n";
    cout << "|                 LUMINES PTHREADS                |\n";
    cout << "+--------------------------------------------------+\n";
    cout << "| 1. Iniciar modo lento                            |\n";
    cout << "| 2. Iniciar modo rapido                           |\n";
    cout << "| 3. Ver instrucciones                             |\n";
    cout << "| 4. Puntajes destacados                           |\n";
    cout << "| 5. Salir                                         |\n";
    cout << "+--------------------------------------------------+\n";
    cout << "Selecciona una opcion: ";
}

int playGame(int mode, int &finalScore, bool &won) {
    GameState state;
    initGame(state, mode);
    pthread_t threads[THREAD_COUNT];

    {
        TerminalMode terminal;
        pthread_create(&threads[0], nullptr, inputThread, &state);
        pthread_create(&threads[1], nullptr, playerThread, &state);
        pthread_create(&threads[2], nullptr, gravityThread, &state);
        pthread_create(&threads[3], nullptr, timelineThread, &state);
        pthread_create(&threads[4], nullptr, detectorThread, &state);
        pthread_create(&threads[5], nullptr, specialThread, &state);
        pthread_create(&threads[6], nullptr, scoreThread, &state);
        pthread_create(&threads[7], nullptr, rendererThread, &state);

        for (pthread_t &thread : threads) {
            pthread_join(thread, nullptr);
        }
    }

    finalScore = state.score;
    won = state.won;
    clearScreen();
    cout << (state.won ? "VICTORIA" : "FIN DEL JUEGO") << "\n";
    cout << "Puntaje final: " << state.score << "\n";
    cout << "Modo: " << modeName(mode) << "\n";
    destroyGame(state);
    return 0;
}

int runSelfTest() {
    GameState state;
    initGame(state, 1);
    state.running = true;

    for (int r = 10; r <= 11; ++r) {
        for (int c = 3; c <= 4; ++c) {
            state.board[r][c].color = 1;
        }
    }
    bool detected = detectSquaresLocked(state);
    if (!detected || state.pendingSquares != 1) {
        cerr << "SELF_TEST_FAIL: no se detecto el cuadrado 2x2.\n";
        destroyGame(state);
        return 1;
    }

    processScoreLocked(state);
    if (state.score < 10) {
        cerr << "SELF_TEST_FAIL: el puntaje no aumento por el cuadrado.\n";
        destroyGame(state);
        return 1;
    }

    state.board[11][4].special = true;
    state.board[11][5].color = 1;
    state.board[11][6].color = 1;
    int expanded = expandSpecialLocked(state);
    if (expanded < 2) {
        cerr << "SELF_TEST_FAIL: el bloque especial no marco la region conectada.\n";
        destroyGame(state);
        return 1;
    }

    int cleared = clearTimelineColumnLocked(state, 4);
    if (cleared == 0) {
        cerr << "SELF_TEST_FAIL: la linea de tiempo no limpio celdas marcadas.\n";
        destroyGame(state);
        return 1;
    }

    destroyGame(state);
    cout << "SELF_TEST_OK: deteccion, puntaje, bloque especial y linea de tiempo funcionan.\n";
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    enableAnsiIfNeeded();
    if (argc > 1 && string(argv[1]) == "--self-test") {
        return runSelfTest();
    }

    while (true) {
        printMenu();
        string option;
        getline(cin, option);
        if (option == "1" || option == "2") {
            int mode = option == "1" ? 1 : 2;
            int finalScore = 0;
            bool won = false;
            playGame(mode, finalScore, won);

            cout << "\nIngresa tu nombre para guardar el puntaje: ";
            string name;
            getline(cin, name);
            if (name.empty()) name = "Jugador";
            saveScore(name, finalScore, modeName(mode), won ? "Victoria" : "Derrota");

            cout << "\nPuntaje guardado. ENTER para volver al menu.";
            string pause;
            getline(cin, pause);
        } else if (option == "3") {
            showInstructions();
        } else if (option == "4") {
            showScores();
        } else if (option == "5" || option == "q" || option == "Q") {
            clearScreen();
            cout << "Gracias por jugar Lumines Pthreads.\n";
            break;
        }
    }
    return 0;
}
