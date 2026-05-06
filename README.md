# 介绍
- Lark_Otter 是 Otter 框架全新版本，全部重构，使其支持跨平台
- 加入了OpenGL与D2D渲染后端可以选择后端实现跨平台等
- 使用全新的设计思路，借鉴了Adobe Photoshop 图层思维实现 图层 图形框架
- 使用链式绘制
- 使用批处理提供更好的性能
- Web渲染提供WebView2和Chrome两个渲染后端实现windows针对和跨平台兼容

# 安装
- 前往 ""释放"" 查看安装包，提供类似 ""easyx"" 的exe安装程序，无需额外配置，管理员运行后自动找到所有的 Visual Studio 版本地址自动安装或者卸载

# 测试

通过此代码测试看是否成功运行

俄罗斯方块：
```cpp
// Otter Tetris - Direct2D backend demo.

#include "OtterWindow.h"
#include <algorithm>
#include <deque>
#include <random>
#include <string>
#include <utility>
#include <vector>

constexpr int COLS = 10;
constexpr int ROWS = 20;
constexpr int CELL_SIZE = 30;
constexpr int BOARD_X = 40;
constexpr int BOARD_Y = 20;
constexpr int SCORE_X = 380;
constexpr int SCORE_Y = 80;
constexpr float FALL_INTERVAL = 0.5f;
constexpr float KEY_REPEAT_INTERVAL = 0.09f;

const int SHAPES[7][4][4] = {
    { {0,0,0,0}, {1,1,1,1}, {0,0,0,0}, {0,0,0,0} }, // I
    { {1,1,0,0}, {1,1,0,0}, {0,0,0,0}, {0,0,0,0} }, // O
    { {0,1,0,0}, {1,1,1,0}, {0,0,0,0}, {0,0,0,0} }, // T
    { {0,1,1,0}, {1,1,0,0}, {0,0,0,0}, {0,0,0,0} }, // S
    { {1,1,0,0}, {0,1,1,0}, {0,0,0,0}, {0,0,0,0} }, // Z
    { {1,0,0,0}, {1,1,1,0}, {0,0,0,0}, {0,0,0,0} }, // J
    { {0,0,1,0}, {1,1,1,0}, {0,0,0,0}, {0,0,0,0} }  // L
};

const Otter::Color COLORS[7] = {
    Otter::Color(0.0f, 0.75f, 0.95f),
    Otter::Color(0.95f, 0.85f, 0.05f),
    Otter::Color(0.60f, 0.20f, 0.95f),
    Otter::Color(0.05f, 0.90f, 0.20f),
    Otter::Color(0.95f, 0.15f, 0.15f),
    Otter::Color(0.20f, 0.40f, 0.95f),
    Otter::Color(0.95f, 0.55f, 0.05f)
};

const Otter::Color BORDER_COLORS[7] = {
    Otter::Color(0.0f, 0.55f, 0.70f),
    Otter::Color(0.70f, 0.60f, 0.05f),
    Otter::Color(0.45f, 0.15f, 0.70f),
    Otter::Color(0.05f, 0.65f, 0.15f),
    Otter::Color(0.70f, 0.10f, 0.10f),
    Otter::Color(0.15f, 0.30f, 0.70f),
    Otter::Color(0.70f, 0.40f, 0.05f)
};

struct Tetromino {
    int type = 0;
    int rotation = 0;
    int x = 0;
    int y = 0;
};

std::vector<std::pair<int, int>> GetCells(const Tetromino& piece) {
    std::vector<std::pair<int, int>> cells;
    const int size = (piece.type == 0) ? 4 : (piece.type == 1 ? 2 : 3);

    for (int outY = 0; outY < size; ++outY) {
        for (int outX = 0; outX < size; ++outX) {
            int srcX = outX;
            int srcY = outY;

            switch (piece.rotation & 3) {
            case 1:
                srcX = size - 1 - outY;
                srcY = outX;
                break;
            case 2:
                srcX = size - 1 - outX;
                srcY = size - 1 - outY;
                break;
            case 3:
                srcX = outY;
                srcY = size - 1 - outX;
                break;
            default:
                break;
            }

            if (SHAPES[piece.type][srcY][srcX] != 0) {
                cells.emplace_back(piece.x + outX, piece.y + outY);
            }
        }
    }

    return cells;
}

class Tetris {
public:
    explicit Tetris(Otter::otterwindow& win)
        : win_(&win), rng_(std::random_device{}()) {
        auto* bg = win.creat["bg"].ptr;
        bg->layer_bounds(0, 0, 550, 680)
            .background(Otter::Color::white());

        boardLayer_ = win.creat["board"].ptr;
        boardLayer_->translate(BOARD_X, BOARD_Y)
            .layer_bounds(0, 0, COLS * CELL_SIZE, ROWS * CELL_SIZE);

        scoreLayer_ = win.creat["score"].ptr;
        scoreLayer_->translate(SCORE_X, SCORE_Y)
            .layer_bounds(0, 0, 200, 130);

        board_.assign(ROWS, std::vector<int>(COLS, -1));
        SpawnPiece();

        win_->set_keyboard_target(
            nullptr,
            [this](Otter::Key key) -> bool {
                if (state_ != GameState::Playing) return false;
                switch (key) {
                case Otter::Key::Left:  PressLeft(); return true;
                case Otter::Key::Right: PressRight(); return true;
                case Otter::Key::Down:  PressDown(); return true;
                case Otter::Key::Up:    PressRotate(); return true;
                case Otter::Key::Space: PressHardDrop(); return true;
                default: return false;
                }
            }
        );

        boardLayer_->on_update([this](float dt) {
            Update(dt);
            return true;
            });

        boardLayer_->on_render([this](Otter::PaintChain& chain, float) {
            DrawBoard(chain);
            DrawCurrentPiece(chain);
            DrawLineClearEffects(chain);
            return true;
            });

        scoreLayer_->on_render([this](Otter::PaintChain& chain, float) {
            DrawScore(chain);
            return true;
            });
    }

private:
    enum class GameState { Playing, LineClearAnim, GameOver };

    struct LineClearData {
        int row = 0;
        float timer = 0.0f;
        float duration = 0.25f;
        bool active = true;
    };

    Otter::otterwindow* win_ = nullptr;
    Otter::Layer* boardLayer_ = nullptr;
    Otter::Layer* scoreLayer_ = nullptr;
    std::vector<std::vector<int>> board_;
    Tetromino currentPiece_;
    std::mt19937 rng_;
    GameState state_ = GameState::Playing;
    std::deque<LineClearData> lineClears_;

    float fallTimer_ = 0.0f;
    float keyPollTimer_ = KEY_REPEAT_INTERVAL;
    int score_ = 0;
    bool leftHeld_ = false;
    bool rightHeld_ = false;
    bool downHeld_ = false;
    bool upHeld_ = false;
    bool spaceHeld_ = false;
    bool keyActionThisFrame_ = false;

    void SpawnPiece() {
        currentPiece_.type = static_cast<int>(rng_() % 7);
        currentPiece_.rotation = 0;
        currentPiece_.x = COLS / 2 - 2;
        currentPiece_.y = 0;

        if (!IsValid(currentPiece_)) {
            state_ = GameState::GameOver;
        }
    }

    bool IsValid(const Tetromino& piece) const {
        for (auto [cx, cy] : GetCells(piece)) {
            if (cx < 0 || cx >= COLS || cy >= ROWS) return false;
            if (cy >= 0 && board_[cy][cx] != -1) return false;
        }
        return true;
    }

    bool TryMove(int dx, int dy) {
        Tetromino moved = currentPiece_;
        moved.x += dx;
        moved.y += dy;
        if (!IsValid(moved)) return false;

        currentPiece_ = moved;
        return true;
    }

    void PressLeft() {
        keyActionThisFrame_ = true;
        TryMove(-1, 0);
    }

    void PressRight() {
        keyActionThisFrame_ = true;
        TryMove(1, 0);
    }

    void PressDown() {
        keyActionThisFrame_ = true;
        MoveDown();
    }

    void PressRotate() {
        keyActionThisFrame_ = true;
        Rotate();
    }

    void PressHardDrop() {
        keyActionThisFrame_ = true;
        HardDrop();
    }

    void MoveDown() {
        if (!TryMove(0, 1)) {
            LockPiece();
        }
    }

    void Rotate() {
        Tetromino rotated = currentPiece_;
        rotated.rotation = (rotated.rotation + 1) & 3;

        if (IsValid(rotated)) {
            currentPiece_ = rotated;
            return;
        }

        static const int kicks[] = { -1, 1, -2, 2 };
        for (int kick : kicks) {
            Tetromino kicked = rotated;
            kicked.x += kick;
            if (IsValid(kicked)) {
                currentPiece_ = kicked;
                return;
            }
        }
    }

    void HardDrop() {
        while (TryMove(0, 1)) {
        }
        LockPiece();
    }

    void LockPiece() {
        for (auto [x, y] : GetCells(currentPiece_)) {
            if (x >= 0 && x < COLS && y >= 0 && y < ROWS) {
                board_[y][x] = currentPiece_.type;
            }
        }

        CheckLines();
        if (state_ != GameState::LineClearAnim) {
            SpawnPiece();
            fallTimer_ = 0.0f;
        }
    }

    void CheckLines() {
        for (int row = 0; row < ROWS; ++row) {
            bool full = true;
            for (int col = 0; col < COLS; ++col) {
                if (board_[row][col] == -1) {
                    full = false;
                    break;
                }
            }

            if (full) {
                lineClears_.push_back(LineClearData{ row });
            }
        }

        if (!lineClears_.empty()) {
            state_ = GameState::LineClearAnim;
            score_ += static_cast<int>(lineClears_.size()) * 100;
        }
    }

    void Update(float dt) {
        if (state_ == GameState::GameOver) return;

        HandleKeyboard(dt);

        if (state_ == GameState::LineClearAnim) {
            UpdateLineClear(dt);
            keyActionThisFrame_ = false;
            return;
        }

        fallTimer_ += dt;
        if (fallTimer_ >= FALL_INTERVAL) {
            fallTimer_ -= FALL_INTERVAL;
            MoveDown();
        }

        keyActionThisFrame_ = false;
    }

    void UpdateLineClear(float dt) {
        bool allDone = true;
        for (auto& lineClear : lineClears_) {
            if (!lineClear.active) continue;

            lineClear.timer += dt;
            if (lineClear.timer >= lineClear.duration) {
                lineClear.active = false;
                RemoveRow(lineClear.row);
            }
            else {
                allDone = false;
            }
        }

        if (!allDone) return;

        lineClears_.clear();
        state_ = GameState::Playing;
        SpawnPiece();
        fallTimer_ = 0.0f;
    }

    void HandleKeyboard(float dt) {
        if (state_ != GameState::Playing) return;

        const bool left = IsKeyDown(Otter::Key::Left) || IsKeyDown('A');
        const bool right = IsKeyDown(Otter::Key::Right) || IsKeyDown('D');
        const bool down = IsKeyDown(Otter::Key::Down) || IsKeyDown('S');
        const bool up = IsKeyDown(Otter::Key::Up) || IsKeyDown('W');
        const bool space = IsKeyDown(Otter::Key::Space);

        if (!keyActionThisFrame_) {
            if (up && !upHeld_) PressRotate();
            if (space && !spaceHeld_) PressHardDrop();

            int dir = 0;
            if (left && !right) dir = -1;
            if (right && !left) dir = 1;

            if (dir != 0 || down) {
                keyPollTimer_ += dt;
                const bool firstPress =
                    (dir < 0 && !leftHeld_) ||
                    (dir > 0 && !rightHeld_) ||
                    (down && !downHeld_);

                if (firstPress || keyPollTimer_ >= KEY_REPEAT_INTERVAL) {
                    keyPollTimer_ = 0.0f;
                    if (dir < 0) PressLeft();
                    if (dir > 0) PressRight();
                    if (down) PressDown();
                }
            }
            else {
                keyPollTimer_ = KEY_REPEAT_INTERVAL;
            }
        }

        leftHeld_ = left;
        rightHeld_ = right;
        downHeld_ = down;
        upHeld_ = up;
        spaceHeld_ = space;
    }

    static bool IsKeyDown(Otter::Key key) {
        return Otter::is_key_down(key);
    }

    static bool IsKeyDown(char key) {
        return Otter::is_key_down(static_cast<Otter::Key>(key));
    }

    void RemoveRow(int row) {
        for (int y = row; y > 0; --y) {
            board_[y] = board_[y - 1];
        }
        board_[0] = std::vector<int>(COLS, -1);
    }

    void DrawBoard(Otter::PaintChain& chain) {
        chain.fill_rect(0, 0, COLS * CELL_SIZE, ROWS * CELL_SIZE, Otter::Color::white());

        Otter::StrokeStyle gridLine;
        gridLine.color = Otter::Color(0.88f, 0.88f, 0.88f);
        gridLine.width = 0.6f;

        for (int i = 0; i <= COLS; ++i) {
            const float x = static_cast<float>(i * CELL_SIZE);
            chain.move_to(x, 0).line_to(x, ROWS * CELL_SIZE).stroke(gridLine);
        }

        for (int i = 0; i <= ROWS; ++i) {
            const float y = static_cast<float>(i * CELL_SIZE);
            chain.move_to(0, y).line_to(COLS * CELL_SIZE, y).stroke(gridLine);
        }

        for (int row = 0; row < ROWS; ++row) {
            for (int col = 0; col < COLS; ++col) {
                const int type = board_[row][col];
                if (type != -1) {
                    DrawGridCell(chain, col, row, COLORS[type], BORDER_COLORS[type]);
                }
            }
        }
    }

    void DrawCurrentPiece(Otter::PaintChain& chain) {
        if (state_ != GameState::Playing) return;

        for (auto [cx, cy] : GetCells(currentPiece_)) {
            if (cy >= 0) {
                DrawGridCell(chain, cx, cy, COLORS[currentPiece_.type], BORDER_COLORS[currentPiece_.type]);
            }
        }
    }

    void DrawLineClearEffects(Otter::PaintChain& chain) {
        for (const auto& lineClear : lineClears_) {
            if (!lineClear.active) continue;

            const float t = std::clamp(lineClear.timer / lineClear.duration, 0.0f, 1.0f);
            const float alpha = 0.8f * (1.0f - t);
            const float y = static_cast<float>(lineClear.row * CELL_SIZE);
            chain.fill_rect(0, y, COLS * CELL_SIZE, CELL_SIZE, Otter::Color(1.0f, 1.0f, 1.0f, alpha));
        }
    }

    void DrawScore(Otter::PaintChain& chain) {
        Otter::TextStyle labelStyle;
        labelStyle.font_family = L"Segoe UI";
        labelStyle.font_size = 26.0f;
        labelStyle.color = Otter::Color::black();
        labelStyle.weight = Otter::TextStyle::Weight::Bold;
        chain.text(L"SCORE", 0, 0, labelStyle);

        Otter::TextStyle scoreStyle;
        scoreStyle.font_family = L"Segoe UI";
        scoreStyle.font_size = 36.0f;
        scoreStyle.color = Otter::Color(0.2f, 0.45f, 0.9f);
        scoreStyle.weight = Otter::TextStyle::Weight::Bold;
        chain.text(std::to_wstring(score_), 0, 40, scoreStyle);

        if (state_ == GameState::GameOver) {
            Otter::TextStyle gameOverStyle;
            gameOverStyle.font_family = L"Segoe UI";
            gameOverStyle.font_size = 20.0f;
            gameOverStyle.color = Otter::Color(0.85f, 0.1f, 0.1f);
            gameOverStyle.weight = Otter::TextStyle::Weight::Bold;
            chain.text(L"GAME OVER", 0, 92, gameOverStyle);
        }
    }

    void DrawGridCell(Otter::PaintChain& chain, int col, int row, Otter::Color fill, Otter::Color border) {
        DrawPixelCell(
            chain,
            static_cast<float>(col * CELL_SIZE),
            static_cast<float>(row * CELL_SIZE),
            fill,
            border);
    }

    void DrawPixelCell(Otter::PaintChain& chain, float x, float y, Otter::Color fill, Otter::Color border) {
        chain.fill_round_rect(x + 1.5f, y + 1.5f, CELL_SIZE - 3, CELL_SIZE - 3, 4.0f, fill);

        Otter::StrokeStyle borderStyle;
        borderStyle.color = border;
        borderStyle.width = 1.8f;
        chain.stroke_round_rect(x + 1.5f, y + 1.5f, CELL_SIZE - 3, CELL_SIZE - 3, 4.0f, borderStyle);

        const Otter::Color light(
            std::min(fill.r + 0.18f, 1.0f),
            std::min(fill.g + 0.18f, 1.0f),
            std::min(fill.b + 0.18f, 1.0f),
            0.5f);
        chain.fill_round_rect(x + 2.5f, y + 2.5f, CELL_SIZE - 8, 4.0f, 2.0f, light);
    }
};

int main() {
    try {
        Otter::otterwindow win(550, 680, L"Otter Tetris");
        Tetris game(win);
        win.run();
    }
    catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Error", MB_ICONERROR);
    }
    return 0;
}

```
