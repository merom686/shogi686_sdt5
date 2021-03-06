#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <array>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <random>
#include <cstdlib>
#include <cstring>
#include <cctype>
using namespace std;

#define DEBUG
#ifdef DEBUG
#define assert(x) \
if (!(x)){ cout << "info string error file:" << __FILE__ << " line:" << __LINE__ << endl; throw; }
#else
#define assert(x) ((void)0)
#endif

using Score = int;

enum Piece {
    Empty, Pawn, Lance, Knight, Silver, Bishop, Rook, Gold,
    King, ProPawn, ProLance, ProKnight, ProSilver, Horse, Dragon,
    PieceTypeNum, HandTypeNum = King, // Numは2冪とは限らない
    PromoteMask = 8, BlackMask = 16, WhiteMask = 32,
};

enum Color {
    Black, White, ColorNum
};
Color operator~(const Color c) {
    return (Color)(c ^ 1);
}

class Option {
public:
    Option(bool value) : idx_(num_++), value_(value ? "true" : "false"), type_("check") {}
    Option(int value, int min, int max) : idx_(num_++), value_(to_string(value)), type_("spin"), min_(min), max_(max) {}
    Option(string value, const vector<string>& var) : idx_(num_++), value_(value), type_("combo"), var_(var) {}
    Option() : idx_(num_++), type_("button") {}
    Option(string value, string type = "string") : idx_(num_++), value_(value), type_(type) {}

    Option& operator=(const string& value) {
        value_ = value;
        return *this;
    }
    operator int() const {
        assert(type_ == "check" || type_ == "spin");
        if (type_ == "check") return value_ == "true";
        return stoi(value_);
    }
    operator string() const {
        assert(type_ == "combo" || type_ == "string" || type_ == "filename");
        return value_;
    }

private:
    friend ostream& operator<<(ostream& os, const Option& option);
    friend ostream& operator<<(ostream& os, const map<string, Option>& options);

    static int num_;
    int idx_;

    string value_, type_;
    int min_, max_;
    vector<string> var_;
};
int Option::num_;

ostream& operator<<(ostream& os, const Option& option) {
    os << "type " << option.type_;
    if (option.type_ != "button") {
        os << " default " << (option.value_.empty() ? "<empty>" : option.value_);
        if (option.type_ == "spin") {
            os << " min " << option.min_ << " max " << option.max_;
        } else if (option.type_ == "combo") {
            for (const auto& var : option.var_) {
                os << " var " << var;
            }
        }
    }
    return os;
}

ostream& operator<<(ostream& os, const map<string, Option>& options) {
    // 登録順に並べ替える
    vector<map<string, Option>::const_iterator> vit(options.size());
    for (auto it = options.cbegin(); it != options.cend(); it++) {
        vit[it->second.idx_] = it;
    }
    for (auto it : vit) {
        cout << "option name " << it->first << ' ' << it->second << '\n';
    }
    return os;
}

// 指し手
class Move {
public:
    Move() {}
    // 移動元(駒台のときは0)8bit, 移動先8bit, 移動前の駒(手番を含まない)4bit, 成ったか1bit, 取った駒4bit
    Move(int from, int to, int pt, int promote, int captured) {
        value_ = from | to << 8 | pt << 16 | promote << 20 | captured << 21;
    }
    static Move None() {
        return Move(0);
    }

    int from() const {
        return value_ & 0xff;
    }
    int to() const {
        return value_ >> 8 & 0xff;
    }
    int piece_type() const {
        return value_ >> 16 & 0xf;
    }
    int promote() const {
        return value_ >> 20 & 0x1;
    }
    int captured() const {
        return value_ >> 21 & 0xf;
    }
    // 移動後の駒
    int piece_to() const {
        return piece_type() | promote() << 3;
    }
    bool is_none() const {
        return value_ == 0;
    }
    bool is_drop() const {
        return from() == 0;
    }
    // 指し手をSFENに変換
    string toSfen() const;

private:
    Move(int v) : value_(v) {}
    int value_;
};

// 定数
constexpr int FileNum = 9, RankNum = 9, PromotionRank = 3;
constexpr int Stride = FileNum + 1, Origin = Stride * 3, SquareNum = Origin + Stride * (RankNum + 2);

constexpr int MaxMove = 593, MaxPly = 64;
const string SfenPiece = "+PLNSBRGK";

constexpr Score ScoreInfinite = numeric_limits<int16_t>::max();
constexpr Score ScoreMate = 32600;
constexpr Score ScoreMateInMaxPly = ScoreMate - MaxPly;

// グローバル変数
chrono::system_clock::time_point time_start, time_end; // 探索を始めた時刻と終了する時刻
mutex mtx;
thread_local Move pv_array[MaxPly][MaxPly]; // 読み筋を記録する
atomic_bool stop;
bool learning = false;
uint64_t nodes;

map<string, Option> options = {
    { "Eval", Option("Default", vector<string>{ "Default", "Random(NoSearch)" }) },
    { "Ordering", Option("Default", vector<string>{ "Default", "Random" }) },
    { "TimeMargin", Option(100, 0, 3000) },
    { "SaveTime", Option(true) },
    { "Mate", Option("Default", vector<string>{ "Default", "Learn", "Average" }) },
};

// 置換表
struct TTEntry {
    uint64_t key;
    int32_t score;
    int8_t depth, bound, padding[2];
};

TTEntry *tt;
uint64_t p2key[SquareNum][32];
size_t tt_size = 1 << 20;

// GetSquare(0, 0)は盤の左上隅(９一)を表す
inline constexpr int GetSquare(int x, int y) {
    return Origin + Stride * y + x;
}

inline constexpr int ColorToTurnMask(int c) {
    return c == Black ? BlackMask : WhiteMask;
}

inline constexpr int TurnMaskToSign(int p) {
    return (p & BlackMask) ? 1 : -1;
}

inline constexpr int ColorToSign(int c) {
    return c == Black ? 1 : -1;
}

// 指定された駒の利きがある全ての升に対して、trueを返すまでfを実行する
template <class F>
inline bool forAttack(const uint8_t *piece, const int sq, const int pt, const int c, F f) {
    constexpr int n = Stride;
    constexpr int att[PieceTypeNum][10] = {
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { -n, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // 歩
        { 0, -n, 0, 0, 0, 0, 0, 0, 0, 0 },
        { -n * 2 + 1, -n * 2 - 1, 0, 0, 0, 0, 0, 0, 0, 0 },
        { -n - 1, -n, -n + 1, n - 1, n + 1, 0, 0, 0, 0, 0 },
        { 0, -n - 1, -n + 1, n - 1, n + 1, 0, 0, 0, 0, 0 },
        { 0, -n, -1, 1, n, 0, 0, 0, 0, 0 },
        { -n - 1, -n, -n + 1, -1, 1, n, 0, 0, 0, 0 }, // 金
        { -n - 1, -n, -n + 1, -1, 1, n - 1, n, n + 1, 0, 0 }, // 玉
        { -n - 1, -n, -n + 1, -1, 1, n, 0, 0, 0, 0 },
        { -n - 1, -n, -n + 1, -1, 1, n, 0, 0, 0, 0 },
        { -n - 1, -n, -n + 1, -1, 1, n, 0, 0, 0, 0 },
        { -n - 1, -n, -n + 1, -1, 1, n, 0, 0, 0, 0 },
        { -n, -1, 1, n, 0, -n - 1, -n + 1, n - 1, n + 1, 0 },
        { -n - 1, -n + 1, n - 1, n + 1, 0, -n, -1, 1, n, 0 }, // 竜
    };

    const int sgn = ColorToSign(c);
    const int *a = att[pt];
    int i;
    for (i = 0; a[i] != 0; i++) {
        if (f(sq + a[i] * sgn)) return true;
    }
    for (i++; a[i] != 0; i++) {
        for (int d = a[i];; d += a[i]) {
            if (f(sq + d * sgn)) return true;
            if (piece[sq + d * sgn] != Empty) break;
        }
    }
    return false;
}

// 局面
struct Position {
    // 局面を比較する 同一ならtrueを返す
    static bool equal(const Position& p1, const Position& p2) {
        return std::equal(p1.piece + Origin, p1.piece + Origin + Stride * RankNum, p2.piece + Origin)
            && std::equal(p1.hand[Black], p1.hand[White], p2.hand[Black])
            && p1.turn == p2.turn;
    }
    // 升が手番にとって敵陣Rank段目までにあるか
    template <int Rank = PromotionRank>
    bool promotionZone(int sq) const {
        if (turn == Black) {
            return sq < GetSquare(0, Rank);
        } else {
            return sq >= GetSquare(0, RankNum - Rank);
        }
    }

    // Positionは初期化してからでないと使えない(コンストラクタで初期化しないのは高速化のため)
    void clear();
    // SFENの局面をセットし現在の局面のポインタを返す
    Position *fromSfen(const string& s);

    // 全ての合法手(王手放置等の反則を含む)を生成し、生成した指し手の個数を返す
    int generateMoves(Move *const moves);
    // cの玉に相手の利きがあるか
    bool inCheck(const Color c) const;
    // 手を進める
    void doMove(const Move move) const;
    // 入玉宣言勝ちの判定
    bool isWin() const;
    // 評価関数
    Score evaluate() const;
    // 局面のハッシュを計算
    void calc_key() {
        uint64_t key = 0;
        for (int y = 0; y < RankNum; y++) {
            for (int x = 0; x < FileNum; x++) {
                int sq = GetSquare(x, y);
                int p = piece[sq];
                if (p == Empty) continue;
                key ^= p2key[sq][p - BlackMask];
            }
        }
        this->key = key + *(uint64_t *)hand[turn] + turn;
    }

    // piece: 駒の種類3bit, 成1bit, 手番1bit 以上5bit;壁は全8bitが立っている
    uint8_t piece[SquareNum], hand[ColorNum][HandTypeNum];
    Color turn;
    int king[ColorNum]; // 玉の位置
    int continuous_check[ColorNum]; // 現在の連続王手回数
    uint64_t key;

    int ply; // Rootからの手数
    Move previous_move; // 直前に指した手
    bool checked; // 手番の玉に王手がかかっているか
};

string Move::toSfen() const {
    if (is_none()) return "resign";

    string s;
    auto add = [&](int sq) {
        sq -= Origin;
        s += '1' + FileNum - 1 - sq % Stride;
        s += 'a' + sq / Stride;
    };

    if (from() == 0) {
        s += SfenPiece[piece_type()];
        s += '*';
        add(to());
    } else {
        add(from());
        add(to());
        if (promote()) s += '+';
    }
    return s;
}

void Position::clear() {
    memset(this, 0, sizeof *this);
    fill_n(piece, SquareNum, 0xff); // 壁で埋める
    for (int y = 0; y < RankNum; y++) {
        fill_n(&piece[GetSquare(0, y)], FileNum, 0); // y段目を全部空き升に
    }
}

Position *Position::fromSfen(const string& s) {
    string sfen_board, sfen_turn, sfen_hand, sfen_count, sfen_move;

    // 局面初期化
    clear();

    // startposを置換してstringstreamを作る
    istringstream iss;
    if (s.compare(0, 8, "startpos") == 0) {
        iss.str("sfen lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1" + s.substr(8));
    } else {
        iss.str(s);
    }

    // 盤面
    iss >> sfen_board; // "sfen"
    iss >> sfen_board;
    int x = 0, y = 0, promote = 0;
    for (char c : sfen_board) {
        if (isdigit(c)) {
            x += c - '0';
        } else if (c == '+') {
            promote = 1;
        } else if (c == '/') {
            x = 0;
            y++;
        } else {
            Color color = (Color)(isupper(c) == 0); // 小文字(後手)のとき1
            if (color == White) c = toupper(c);
            size_t i = SfenPiece.find(c);
            assert(i != string::npos && i != Empty && i <= King);
            int p = (int)i + promote * PromoteMask;
            int sq = GetSquare(x, y);
            piece[sq] = p | ColorToTurnMask(color);
            promote = 0;
            x++;
            if (p == King) king[color] = sq;
        }
    }

    // 手番
    iss >> sfen_turn;
    turn = sfen_turn == "b" ? Black : White;

    // 持ち駒
    iss >> sfen_hand;
    int k = 0;
    for (auto c : sfen_hand) {
        if (c == '-') {
            break;
        } else if (isdigit(c)) {
            k = k * 10 + (c - '0');
        } else {
            Color color = (Color)(isupper(c) == 0);
            if (color == White) c = toupper(c);
            size_t i = SfenPiece.find(c);
            assert(i != string::npos && i != Empty && i < King);
            hand[color][i] = k == 0 ? 1 : k;
            k = 0;
        }
    }

    // 手数(使わない)
    iss >> sfen_count;

    checked = inCheck(turn);
    calc_key();

    // 指し手
    iss >> sfen_move;
    if (sfen_move != "moves") return this;

    Position *ppos = this;
    while (iss >> sfen_move) {
        // 全ての合法手を生成して一致するものを探す(合法手でないものを生成しないとは言っていない)
        Move moves[MaxMove];
        int n = ppos->generateMoves(moves);

        auto it = find_if(moves, moves + n, [&](Move move) {
            return sfen_move == move.toSfen();
        });
        assert(it < moves + n);
        (ppos++)->doMove(*it);
    }
    return ppos;
}

int Position::generateMoves(Move *const moves) {
    const int turn_mask = ColorToTurnMask(turn);
    Move *m = moves;
    int pawn = 0; // 二歩検出用のビットマップ
                  // 移動
    for (int y = 0; y < RankNum; y++) {
        for (int x = 0; x < FileNum; x++) {
            const int from = GetSquare(x, y);
            const int p = piece[from];
            if (p & turn_mask) {
                int pt = p % BlackMask;
                if (pt == Pawn) pawn |= 1 << x;
                forAttack(piece, from, pt, turn, [&](int to) {
                    int captured = piece[to];
                    // 自分の駒と壁以外(空升と相手の駒)へなら移動できる
                    if (!(captured & turn_mask)) {
                        if (pt < Gold && (promotionZone(from) || promotionZone(to))) {
                            *m++ = Move(from, to, pt, 1, captured % BlackMask);
                        }
                        if (!((pt == Pawn || pt == Lance) && promotionZone<1>(to))
                            && !(pt == Knight && promotionZone<2>(to))) {
                            *m++ = Move(from, to, pt, 0, captured % BlackMask);
                        }
                    }
                    return false;
                });
            }
        }
    }
    // 打つ
    for (int pt = Pawn; pt < HandTypeNum; pt++) {
        if (!hand[turn][pt]) continue;
        for (int y = 0; y < RankNum; y++) {
            for (int x = 0; x < FileNum; x++) {
                int to = GetSquare(x, y);
                int p = piece[to];
                if (p == Empty && !(pt == Pawn && (pawn & 1 << x))) {
                    if (!((pt == Pawn || pt == Lance) && promotionZone<1>(to))
                        && !(pt == Knight && promotionZone<2>(to))) {
                        *m++ = Move(0, to, pt, 0, 0);
                    }
                }
            }
        }
    }
    return (int)(m - moves);
}

bool Position::inCheck(const Color c) const {
    for (int pt = Pawn; pt < PieceTypeNum; pt++) {
        const int p = pt | ColorToTurnMask(~c);
        bool ret = forAttack(piece, king[c], pt, c, [&](int sq) {
            return piece[sq] == p;
        });
        if (ret) return true;
    }
    return false;
}

void Position::doMove(const Move move) const {
    Position& pos = const_cast<Position *>(this)[1];
    pos = *this; // 次の深さへコピー thisはいじらない

    if (move.is_drop()) {
        // 打つ
        pos.hand[this->turn][move.piece_type()]--;
        pos.piece[move.to()] = move.piece_type() | ColorToTurnMask(this->turn);

    } else {
        // 移動
        if (move.captured()) {
            // 取る
            pos.hand[this->turn][move.captured() % PromoteMask]++;
        }
        pos.piece[move.from()] = Empty;
        pos.piece[move.to()] = move.piece_to() | ColorToTurnMask(this->turn); // 取る手の場合は上書き
        if (move.piece_type() == King) pos.king[this->turn] = move.to();
    }
    pos.turn = ~pos.turn;
    pos.ply++;
    pos.calc_key();

    // いま指した手
    pos.previous_move = move;
    // いま指した手が王手だったか
    pos.checked = pos.inCheck(pos.turn);
    // 連続王手の回数を更新
    if (pos.checked) {
        pos.continuous_check[this->turn]++;
    } else {
        pos.continuous_check[this->turn] = 0;
    }
}

bool Position::isWin() const {
    if (!promotionZone(king[turn])) return false;
    if (checked) return false;

    constexpr int PieceScore[PieceTypeNum] = {
        0, 1, 1, 1, 1, 5, 5, 1,
        0, 1, 1, 1, 1, 5, 5, };
    int n = 0, score = 0;

    int turn_mask = ColorToTurnMask(turn);
    int y1 = turn == Black ? 3 : 9;
    for (int y = y1 - 3; y < y1; y++) {
        for (int x = 0; x < FileNum; x++) {
            int sq = GetSquare(x, y);
            int p = piece[sq];
            if (!(p & turn_mask)) continue;

            int pt = p % BlackMask;
            if (pt != King) {
                score += PieceScore[pt];
                n++;
            }
        }
    }
    if (n < 10) return false;

    for (int p = Pawn; p < HandTypeNum; p++) {
        score += PieceScore[p] * hand[turn][p];
    }
    return score >= (turn == Black ? 28 : 27);
}

constexpr int pn[PieceTypeNum + 1] = { -1, 1, 2, 3, 4, 5, 6, 7, 0, 7, 7, 7, 7, 8, 9, 10 };
constexpr int hn[HandTypeNum + 1] = { -1, 0, 18, 22, 26, 30, 32, 34, 38 };
constexpr int p1 = pn[PieceTypeNum] * (RankNum * FileNum), p2 = p1 + hn[HandTypeNum], p3 = p2 * 2;
constexpr int FvScale = 32, PPSize = p3 * p3;
int16_t (*pp)[p3];

Score Position::evaluate() const {
    // 駒割
    constexpr int PieceScore[PieceTypeNum] = {
        0, 100, 300, 300, 400, 700, 800, /*金*/500,
        0, 600, 500, 500, 500, 800, 1000, };

    // 先手から見た点数
    int score = 0;

    int pl[40], h = 0;

    // 盤上の駒
    for (int y = 0; y < RankNum; y++) {
        for (int x = 0; x < FileNum; x++) {
            int sq = GetSquare(x, y);
            int p = piece[sq];
            if (p == Empty) continue;

            int pt = p % BlackMask;
            int sgn = TurnMaskToSign(p);
            score += sgn * PieceScore[pt];

            pl[h++] = pn[pt] * (RankNum * FileNum) + (FileNum * y + x) + (sgn < 0) * p2;
        }
    }

    // 持ち駒
    int sum[ColorNum] = {};
    for (int c = 0; c < ColorNum; c++) {
        for (int pt = Pawn; pt < HandTypeNum; pt++) {
            sum[c] += hand[c][pt] * PieceScore[pt];

            for (int i = 0; i < hand[c][pt]; i++) {
                pl[h++] = p1 + hn[pt] + i + c * p2;
            }
        }
    }
    score += sum[0] - sum[1];

    assert(h == 40);
    score *= FvScale;
    for (int i = 0; i < h; i++) {
        int16_t *p = pp[pl[i]];
        for (int j = 0; j < i; j++) {
            score += p[pl[j]];
        }
    }
    score /= FvScale;

    // 手番から見た点数を返す
    return score * ColorToSign(turn);
}

// 読み筋などの情報をGUIに送る
void infoToUSI(const Score score, const int depth) {
    auto duration = chrono::system_clock::now() - ::time_start; // 経過した時間
    auto msec = chrono::duration_cast<chrono::milliseconds>(duration).count(); // 経過した時間(ミリ秒単位)
    if (msec == 0) msec = 1;

    ostringstream oss_score;
    if (abs(score) >= ScoreMateInMaxPly) {
        if (score > 0) {
            oss_score << "mate +" << ScoreMate - score;
        } else {
            oss_score << "mate -" << ScoreMate + score;
        }
    } else {
        oss_score << "cp " << score;
    }

    string pv;
    for (int i = 0; i < MaxPly; i++) {
        Move move = ::pv_array[0][i];
        if (move.is_none()) break;
        pv += ' ' + move.toSfen();
    }

    cout << "info" << " depth " << depth << " time " << msec << " nodes " << ::nodes
        << " nps " << ::nodes * 1000 / msec << " score " << oss_score.str() << " pv" << pv << endl;
}

// 探索 静止探索を含む 静止探索は取る手深さ4と王手回避(リキャプチャも入れたい)
Score search(Position &pos, Score alpha, const Score beta, const int depth) {
    ::pv_array[pos.ply][0] = Move::None();
    ::nodes++;

    TTEntry *tte = &tt[pos.key & (tt_size - 1)];

    if (pos.ply > 0) {
        // 千日手 16手まで遡る
        for (int i = 4; i <= 16; i += 2) {
            const int64_t diff = pos.key - (&pos - i)->key;
            if (diff == 0) {
                if (pos.continuous_check[ pos.turn] * 2 >= i) return -ScoreInfinite;
                if (pos.continuous_check[~pos.turn] * 2 >= i) return +ScoreInfinite;
                return 0;
            }
            constexpr int64_t Mask = 0x0101010101010100 * 3;
            if ((+diff & ~Mask) == 0) return +ScoreInfinite;
            if ((-diff & ~Mask) == 0) return -ScoreInfinite;
        }

        // 置換表
        if (!learning && tte->key == pos.key) {
            if (depth <= tte->depth) {
                if (tte->bound == 3 || (tte->bound == 2 && tte->score >= beta) || (tte->bound == 1 && tte->score <= alpha)) {
                    return tte->score; // 経路依存のスコアもあるのでほんとはよくない
                }
            }
        }
    }

    Score best_score = -ScoreMate + pos.ply;
    if (pos.isWin()) return -best_score;

    const bool QSearch = depth <= 0 && !pos.checked; // 静止探索か
    if (QSearch) {
        best_score = pos.evaluate();
        if (best_score >= beta || depth <= -4) return best_score;
    }

    Move moves[MaxMove];
    int n = pos.generateMoves(moves);
    bool no_legal = true; // まだこの局面で合法手が見つかっていない
    Score alpha0 = alpha;

    if (pos.ply == 0 && (string)::options["Ordering"] == "Random") {
        // 毎回同じ将棋にならないように Rootのみなので遅くていい
        shuffle(moves, moves + n, random_device());
    }

    for (int i = 0; i < n; i++) {
        Move move = moves[i];
        if (QSearch && !move.captured()) continue; // 静止探索は取る手だけ

        // 手を進める
        pos.doMove(move);
        // 王手放置でないか確かめる(打ち歩詰め等の可能性は残っている)
        if ((&pos + 1)->inCheck(pos.turn)) continue;

        Score score = -search(*(&pos + 1), -beta, -alpha, depth - 1 + ((&pos + 1)->checked && !QSearch));
        no_legal = false;

        // 手を戻す必要はない

        if (score > best_score) {
            best_score = score;

            if (score > alpha) {
                // PVをコピーする
                ::pv_array[pos.ply][0] = move;
                for (int j = 1; j < MaxPly; j++) {
                    if ((::pv_array[pos.ply][j] = ::pv_array[pos.ply + 1][j - 1]).is_none()) break;
                }
                if (score >= beta) goto exit; // βカット
                alpha = score;
            }
        }

        // 思考中断
        if (!learning && (::stop || chrono::system_clock::now() >= ::time_end)) {
            ::stop = true;
            return 0; // stopがtrueのときはスコアを使わないので適当に返す
        }
    }

    // 打ち歩詰め
    if (!QSearch && no_legal && pos.checked) {
        if (pos.previous_move.is_drop() && pos.previous_move.piece_type() == Pawn) return ScoreInfinite;
    }

exit:
    if (!learning) {
        tte->key = pos.key;
        tte->score = best_score;
        tte->depth = depth;
        tte->bound = best_score <= alpha0 ? 1 : best_score >= beta ? 2 : 3;
    }

    return best_score;
}

// 合法手の中からランダムに選んで返す
Move randomMove(Position& pos) {
    Move moves[MaxMove];
    int n = pos.generateMoves(moves);
    uniform_int_distribution<int> distribution(0, n - 1);
    random_device rand;
    int k = distribution(rand);

    for (int i = 0; i < n; i++) {
        Move move = moves[(i + k) % n];
        pos.doMove(move);
        if ((&pos + 1)->inCheck(pos.turn)) continue;
        Score score = search(*(&pos + 1), -ScoreInfinite, ScoreInfinite, 0);
        if (score == ScoreInfinite) continue; // 打ち歩詰め、連続王手の千日手(同一局面2回目でも反則とみなす)
        return move;
    }
    return Move::None();
}

// 反復深化
void idLoop(Position *const ppos) {
    Move best_move = Move::None();

    if (ppos->isWin()) {
        cout << "info score mate + string nyugyoku win" << endl;
        cout << "bestmove win" << endl;
        return;
    }
    if ((string)::options["Eval"] == "Random(NoSearch)") goto id_end;

    for (int depth = 1; depth < MaxPly; depth++) {
        Score score = search(*ppos, -ScoreInfinite, ScoreInfinite, depth);
        if (::stop) break;

        best_move = ::pv_array[0][0];
        infoToUSI(score, depth);
        if (abs(score) >= ScoreMateInMaxPly) break;

        if (::options["SaveTime"]) {
            auto t = chrono::system_clock::now();
            auto d0 = t - ::time_start;
            auto d1 = ::time_end - t;
            if (d1 < d0 * 5) break;
        }
    }
id_end:
    // 時間までに1手も読めなかったらランダムに指す
    if (best_move.is_none()) best_move = randomMove(*ppos);

    if (best_move.is_none()) cout << "info score mate - string resign" << endl;

    cout << "bestmove " << best_move.toSfen() << endl;
}

void think(Position& pos, const int msec) {
    ::time_start = chrono::system_clock::now();
    ::time_end = ::time_start + chrono::milliseconds(msec - ::options["TimeMargin"]);
    ::stop = false;
    ::nodes = 0;

    pos.ply = 0;

    thread thread(idLoop, &pos);
    thread.detach();
}

// 評価ベクトルの学習
float (*g_pp)[p3], (*g2_pp)[p3];
float eta = 30.f;

int rotate180(int pi) {
    int c = pi >= p2; if (c) pi -= p2;
    if (pi < p1) {
        int sq = pi % (RankNum * FileNum);
        pi += (RankNum * FileNum) - 1 - sq * 2;
    }
    if (!c) pi += p2;
    return pi;
}

constexpr float A = 0.0016f;
float sc2wp(Score score) {
    return 1.f / (1.f + exp(-A * score));
}

void learn(const Position& startpos) {
    // depth2の対局をたくさんやって1000局面に達したら更新する adagradを使う 対局中は各局面でスコアと静止探索の末端局面とスコアを保存しておく
    // 対局は、最初の10手までランダムに指したり指さなかったりして、最後にランダムに指したより後の局面のみを学習に使う
    // 対局が終わったらPR文書の方法で各局面の勝率を推定し、静止探索の勝率との差の2乗の和を最小化するように末端局面の特徴へ勾配を与える
    // 2駒関係の対称性 左右対称は考えない 2駒入れ替えと回転対称くらいかな 回転対称のPPはゼロ 2駒を入れ替えたもの、回転させたもの、その4通りに勾配を与える 自分自身との関係は無し
    // ある局面で自分が-100と評価し、次の局面からずっと-1000とかになってたとする。-100の局面では別の手段があるかもしれないしないかもしれない。
    // いっぽう、+1000になったなら-100の局面は実は+1000だったということになる。まあそれはいいか。

    cout << "info string learn" << endl;

    learning = true;

    int thread_num = 8;
    thread threads[8];

    mt19937_64 rnd[8];
    for (int i = 0; i < 8; i++) {
        rnd[i] = mt19937_64(i);
    }

    int depth = 2;

    vector<Position> vpos(thread_num * 1024);
    vector<Position> vposl(thread_num * 1024);
    vector<array<Score, 2>> vscorel(thread_num * 1024);

    constexpr size_t g_pp_size = PPSize * sizeof(**g_pp);
    g_pp = (decltype(g_pp))malloc(g_pp_size);
    g2_pp = (decltype(g_pp))calloc(1, g_pp_size);

    for (int e = 0;; e++) {
        memset(g_pp, 0, g_pp_size);

        atomic_int pos_num = 0;
        for (int i = 0; i < thread_num; i++) {
            threads[i] = thread([&](int i) {
                while (pos_num < 1000) {
                    Position *ppos = &vpos[i * 1024 + 16];
                    Position *pposl = &vposl[i * 1024 + 16];
                    auto pscorel = &vscorel[i * 1024 + 16]; // 先手から見たスコア

                    *ppos = startpos;
                    float result;

                    Score score = 0, qscore = 0;
                    int kt = rnd[i]() % 2;
                    int k0 = -1;

                    // 対局
                    int k;
                    for (k = 0;; k++) {
                        ppos->ply = 0;
                        Move best_move = Move::None();

                        if (k < 10/* && k % 2 == kt*/ && (k < 4 || rnd[i]() % 8 > 0)) {
                            k0 = k;
                            best_move = randomMove(*ppos);

                        } else {
                            score = search(*ppos, -ScoreInfinite, ScoreInfinite, depth);
                            best_move = ::pv_array[0][0];

                            qscore = search(*ppos, -ScoreInfinite, ScoreInfinite, 0);
                            for (int i = 0; i < MaxPly; i++) {
                                Move move = ::pv_array[0][i];
                                if (move.is_none()) {
                                    pposl[k] = ppos[i];
                                    break;
                                }
                                ppos[i].doMove(move);
                            }

                            int sgn = ColorToSign(ppos->turn);
                            pscorel[k][0] = qscore * sgn;
                            pscorel[k][1] = score * sgn;
                        }

                        if (abs(score) >= ScoreMateInMaxPly || best_move.is_none() || k >= 300) {
                            if (ppos->turn != Black) score *= -1;
                            if (score >= ScoreMateInMaxPly) {
                                result = 1.f;
                            } else if (score <= -ScoreMateInMaxPly) {
                                result = 0.f;
                            } else {
                                result = 0.5f;
                            }
                            break;
                        }

                        ppos->doMove(best_move);
                        ppos++;
                    }

                    // 勾配
                    lock_guard<mutex> lock(::mtx);

                    pos_num += k - k0;
                    for (; k > k0; k--) {
                        Position& pos = pposl[k]; // 静止探索の末端
                        auto& sa = pscorel[k];

                        constexpr float P = 0.6f;
                        result = result * P + sc2wp(sa[1]) * (1.f - P); // 先手からみた推定勝率
                        float t = sc2wp(sa[0]);
                        float g = 2 * A * t * (1 - t) * (t - result);

                        int pl[40], h = 0;

                        // 盤上の駒
                        for (int y = 0; y < RankNum; y++) {
                            for (int x = 0; x < FileNum; x++) {
                                int sq = GetSquare(x, y);
                                int p = pos.piece[sq];
                                if (p == Empty) continue;

                                int pt = p % BlackMask;
                                int sgn = TurnMaskToSign(p);

                                pl[h++] = pn[pt] * (RankNum * FileNum) + (FileNum * y + x) + (sgn < 0) * p2;
                            }
                        }

                        // 持ち駒
                        for (int c = 0; c < ColorNum; c++) {
                            for (int pt = Pawn; pt < HandTypeNum; pt++) {
                                for (int i = 0; i < pos.hand[c][pt]; i++) {
                                    pl[h++] = p1 + hn[pt] + i + c * p2;
                                }
                            }
                        }

                        assert(h == 40);
                        for (int i = 0; i < h; i++) {
                            float *p = g_pp[pl[i]];
                            for (int j = 0; j < i; j++) {
                                p[pl[j]] += g;
                            }
                        }
                    }
                }

            }, i);
        }
        for (int i = 0; i < thread_num; i++) {
            threads[i].join();
        }

        // 評価ベクトルの更新
        for (int pi0 = 0; pi0 < p3; pi0++) {
            for (int pi1 = 0; pi1 < p3; pi1++) {
                if (pi0 < pi1) {
                    int pi2 = rotate180(pi0);
                    int pi3 = rotate180(pi1);
                    float t = g_pp[pi0][pi1] + g_pp[pi1][pi0] - g_pp[pi2][pi3] - g_pp[pi3][pi2];
                    g_pp[pi0][pi1] = g_pp[pi1][pi0] = t;
                    g_pp[pi2][pi3] = g_pp[pi3][pi2] = -t;
                }
            }
        }
        int m0 = 1 << 30, m1 = -m0;
        for (int pi0 = 0; pi0 < p3; pi0++) {
            for (int pi1 = 0; pi1 < p3; pi1++) {
                float g = g_pp[pi0][pi1];
                if (g == 0) continue;

                g2_pp[pi0][pi1] += g * g;
                int t = (int)(eta * g / sqrt(g2_pp[pi0][pi1]) + (g > 0 ? 1 : -1) * 0.5f);
                pp[pi0][pi1] -= t;

                if (pp[pi0][pi1] < m0) m0 = pp[pi0][pi1];
                if (pp[pi0][pi1] > m1) m1 = pp[pi0][pi1];
            }
        }

        printf("info string %d %d %d %d\n", e, pos_num.load(), m0, m1);
        cout << flush;

        // 保存
        if ((e + 1) % 25 == 0) {
            for (int i = 0; i < 900; i++) {
                string name = "pp_" + to_string(100 + i) + ".bin";
                fstream fs;
                fs.open(name, ios::in | ios::binary);
                if (fs.is_open()) { fs.close(); continue; }
                rename("pp.bin", name.c_str());
                fs.open("pp.bin", ios::out | ios::binary);
                if (!fs.is_open()) continue;
                fs.write(reinterpret_cast<char *>(pp), PPSize * sizeof **pp);
                break;
            }
        }
    }
}

void average() {
    int32_t (*pp2)[p3];
    pp2 = (decltype(pp2))calloc(1, PPSize * sizeof(**pp2));

    int n = 57;
    for (int i = 0; i < n; i++) {
        fstream fs;
        string name = "pp_" + to_string(100 + i) + ".bin";
        fs.open(name, ios::in | ios::binary);
        if (fs.is_open()) {
            fs.read(reinterpret_cast<char *>(pp), PPSize * sizeof **pp);
            fs.close();
        } else {
            throw;
        }
        for (int i = 0; i < PPSize; i++) {
            pp2[0][i] += pp[0][i];
        }
    }
    for (int i = 0; i < PPSize; i++) {
        pp[0][i] = (int16_t)round((double)pp2[0][i] / n);
    }
    fstream fs;
    fs.open("pp.bin", ios::out | ios::binary);
    if (!fs.is_open()) throw;
    fs.write(reinterpret_cast<char *>(pp), PPSize * sizeof **pp);
}

void isready() {
    pp = (decltype(pp))malloc(PPSize * sizeof(**pp));
    fstream fs;
    fs.open("pp.bin", ios::in | ios::binary);
    if (fs.is_open()) {
        fs.read(reinterpret_cast<char *>(pp), PPSize * sizeof **pp);
        fs.close();
    }

    tt = (TTEntry *)calloc(tt_size, sizeof TTEntry);

    mt19937_64 rnd(686);
    for (int i = 0; i < SquareNum; i++) {
        for (int j = 0; j < 32; j++) {
            p2key[i][j] = rnd();
        }
    }
}

// UIからのコマンドを受け取る
void usiLoop() {
    vector<Position> vpos(1024);
    Position *ppos = &vpos[16];

    string cmd, token;

    while (getline(cin, cmd)) {
        istringstream iss(cmd);
        iss >> token;

        if (token == "usi") {
            cout << "id name shogi686_sdt5" << endl;
            cout << "id author merom686" << endl;
            cout << ::options;
            cout << "usiok" << endl;

        } else if (token == "setoption") {
            string name, value;
            iss >> token; // "name"
            iss >> name;
            iss >> token; // "value"
            value = iss.str().substr((size_t)iss.tellg() + 1);
            if (::options.count(name) > 0) ::options[name] = value; // 空文字列の意味で"<empty>"が送られて来ることはないよね？

        } else if (token == "isready") {
            isready();
            cout << "readyok" << endl;

        } else if (token == "position") {
            ppos = vpos[16].fromSfen(iss.str().substr((size_t)iss.tellg() + 1));

        } else if (token == "go") {
            Position& pos = *ppos;
            iss >> token;

            if (token == "btime") {
                int btime, wtime, byoyomi;
                iss >> btime >> token >> wtime >> token >> byoyomi;
                int time = pos.turn == Black ? btime : wtime;
                think(pos, max((time / 30 + byoyomi) / 1000 * 1000, 1000));

            } else if (token == "infinite") {
                cout << "info score cp " << pos.evaluate() << " string static score" << endl;
                think(pos, 86400 * 1000);

            } else if (token == "mate") {
                if ((string)::options["Mate"] == "Learn") {
                    learn(*ppos);
                } else if ((string)::options["Mate"] == "Average") {
                    average();
                } else {
                    cout << "checkmate notimplemented" << endl;
                }
            }

        } else if (token == "stop") {
            ::stop = true;

        } else if (token == "quit") {
            ::stop = true;
            break;

        } else {
            // 他(usinewgame, ponderhit, gameover)は聞き流す
        }
    }
}

int main() {
    usiLoop();
    return 0;
}
