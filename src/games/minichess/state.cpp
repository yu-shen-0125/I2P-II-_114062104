#include <iostream>
#include <sstream>
#include <cstdint>
#include <cstdlib>

#include "./state.hpp"
#include "config.hpp"
#include "../../policy/game_history.hpp"


/*============================================================
 * KP (King-Piece) Evaluation tables
 *============================================================*/

static const int kp_material[7] = {0, 100, 500, 300, 320, 900, 10000};
static const int simple_material[7] = {0, 2, 6, 7, 8, 20, 100};

static const int pst[6][BOARD_H][BOARD_W] = {
    // Pawn — strong advancement bonus, central files preferred
    {{ 0,  0,  0,  0,  0}, {20, 22, 22, 22, 20}, { 6,  8, 12,  8,  6},
     { 3,  5,  7,  5,  3}, { 0,  2,  2,  2,  0}, { 0,  0,  0,  0,  0}},
    // Rook — open files, advanced ranks
    {{ 3,  3,  4,  3,  3}, { 5,  5,  6,  5,  5}, { 0,  0,  3,  0,  0},
     { 0,  0,  3,  0,  0}, { 0,  0,  2,  0,  0}, {-1,  0,  0,  0, -1}},
    // Knight — centre dominant
    {{-6, -3,  0, -3, -6}, {-3,  3,  5,  3, -3}, { 0,  5,  8,  5,  0},
     { 0,  5,  8,  5,  0}, {-3,  3,  5,  3, -3}, {-6, -3,  0, -3, -6}},
    // Bishop — long diagonals
    {{-2,  0,  0,  0, -2}, { 0,  4,  5,  4,  0}, { 0,  5,  5,  5,  0},
     { 0,  5,  5,  5,  0}, { 0,  4,  5,  4,  0}, {-2,  0,  0,  0, -2}},
    // Queen — flexible central
    {{-2,  0,  2,  0, -2}, { 0,  3,  5,  3,  0}, { 0,  5,  7,  5,  0},
     { 0,  5,  7,  5,  0}, { 0,  3,  5,  3,  0}, {-2,  0,  2,  0, -2}},
    // King — stay back early, hide on flank
    {{-6, -6, -8, -6, -6}, {-4, -4, -6, -4, -4}, {-4, -4, -6, -4, -4},
     {-3, -3, -5, -3, -3}, { 5,  5,  0,  5,  5}, { 8,  8,  2,  8,  8}},
};

static const int tropism_w[7] = {0, 0, 4, 4, 3, 6, 0};

static int king_tropism(int piece_type, int pr, int pc, int ekr, int ekc){
    int dist = std::max(std::abs(pr - ekr), std::abs(pc - ekc));
    if(dist <= 2){
        return tropism_w[piece_type] * (3 - dist);
    }
    return 0;
}

static int pawn_promotion_bonus(int owner, int row){
    int dist = owner == 0 ? row : (BOARD_H - 1 - row);
    switch(dist){
        case 1: return 160;
        case 2: return 55;
        case 3: return 20;
        default: return 0;
    }
}

static int pawn_position_bonus(int owner, int row, int col){
    int dist = owner == 0 ? row : (BOARD_H - 1 - row);
    int center_dist2 = std::abs(2 * col - (BOARD_W - 1));
    int score = pawn_promotion_bonus(owner, row) + 8 - 3 * center_dist2;
    if((col == 0 || col == BOARD_W - 1) && dist >= 3){
        score -= 35;
    }
    return score;
}

static int material_total(const Board& board, int owner){
    int total = 0;
    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            total += simple_material[(int)board.board[owner][r][c]];
        }
    }
    return total;
}

static int max_step_material_bonus(const Board& board, int player, int step){
    const int start = MAX_STEP - 15;
    if(step <= start){ return 0; }
    int self_mat = material_total(board, player);
    int oppn_mat = material_total(board, 1 - player);
    int urgency = step - start;
    return (self_mat - oppn_mat) * urgency * 3;
}

static int development_score(const Board& board, int owner){
    int home = owner == 0 ? BOARD_H - 1 : 0;
    int score = 0;
    int developed_minors = 0;

    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int piece = board.board[owner][r][c];
            if(!piece){
                continue;
            }
            if((piece == 3 || piece == 4) && r != home){
                score += 45;
                developed_minors++;
            }else if(piece == 2 && r != home){
                score += 18;
            }else if(piece == 5 && r != home && developed_minors == 0){
                score -= 25;
            }

            if(piece == 1){
                int dist = owner == 0 ? r : (BOARD_H - 1 - r);
                if(developed_minors == 0 && dist <= 3){
                    score -= (c == 0 || c == BOARD_W - 1) ? 180 : 90;
                }
                if(dist <= 2 && (c == 0 || c == BOARD_W - 1)){
                    score -= 120;
                }
            }
        }
    }

    return score;
}

static bool in_bounds(int r, int c){
    return r >= 0 && r < BOARD_H && c >= 0 && c < BOARD_W;
}

static bool occupied(const Board& board, int r, int c){
    return board.board[0][r][c] || board.board[1][r][c];
}

static bool attacks_square(
    const Board& board, int owner,
    int from_r, int from_c, int target_r, int target_c
){
    int piece = board.board[owner][from_r][from_c];
    if(!piece || (from_r == target_r && from_c == target_c)) return false;
    int dr = target_r - from_r, dc = target_c - from_c;
    int adr = std::abs(dr), adc = std::abs(dc);
    if(piece == 1){ int fwd = owner==0?-1:1; return dr==fwd && adc==1; }
    if(piece == 3){ return (adr==1&&adc==2)||(adr==2&&adc==1); }
    if(piece == 6){ return std::max(adr,adc)==1; }
    bool rook_line = (dr==0||dc==0), bishop_line = (adr==adc);
    if(piece==2&&!rook_line) return false;
    if(piece==4&&!bishop_line) return false;
    if(piece==5&&!rook_line&&!bishop_line) return false;
    int step_r = (dr>0)-(dr<0), step_c = (dc>0)-(dc<0);
    int r = from_r+step_r, c = from_c+step_c;
    while(r!=target_r||c!=target_c){
        if(occupied(board,r,c)) return false;
        r+=step_r; c+=step_c;
    }
    return true;
}

static int count_attackers(const Board& board, int owner, int target_r, int target_c){
    int count = 0;
    for(int r = 0; r < BOARD_H; r++)
        for(int c = 0; c < BOARD_W; c++)
            if(attacks_square(board, owner, r, c, target_r, target_c))
                count++;
    return count;
}

static int piece_relation_score(const Board& board, int owner){
    int oppn = 1 - owner;
    int score = 0;
    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int piece = board.board[owner][r][c];
            if(!piece || piece == 6) continue;
            int value = PIECE_VALUES[piece];
            int attacked = count_attackers(board, oppn, r, c);
            int defended = count_attackers(board, owner, r, c);
            if(defended) score += 2 * value;
            if(attacked){
                score -= defended ? 4*value : 12*value;
                if(piece==5) score -= defended ? 3600 : 15000;
                else if(piece==2) score -= defended ? 1200 : 4200;
            }
        }
    }
    return score;
}

static int king_zone_pressure(const Board& board, int attacker, int king_r, int king_c){
    if(king_r < 0) return 0;
    int pressure = 0;
    for(int dr = -1; dr <= 1; dr++){
        for(int dc = -1; dc <= 1; dc++){
            int r = king_r+dr, c = king_c+dc;
            if(!in_bounds(r,c)) continue;
            int attackers = count_attackers(board, attacker, r, c);
            pressure += attackers * (dr==0&&dc==0 ? 200 : 55);
        }
    }
    return pressure;
}

struct MobilityInfo {
    int mobility = 0;
    int tactics = 0;
    bool can_capture_king = false;
};

static MobilityInfo compute_mobility_info(const Board& board, int side){
    MobilityInfo info;
    auto& self_board = board.board[side];
    auto& oppn_board = board.board[1 - side];

    for(int i = 0; i < BOARD_H; i++){
        for(int j = 0; j < BOARD_W; j++){
            int piece = self_board[i][j];
            if(!piece) continue;
            switch(piece){
                case 1: {
                    int forward = side==0?-1:1;
                    int nr = i+forward;
                    if(nr>=0&&nr<BOARD_H){
                        if(!self_board[nr][j]&&!oppn_board[nr][j]){
                            info.mobility++;
                            if(nr==0||nr==BOARD_H-1) info.tactics+=60;
                        }
                        for(int dc:{-1,1}){
                            int nc=j+dc;
                            if(nc<0||nc>=BOARD_W) continue;
                            int target=oppn_board[nr][nc];
                            if(target){
                                info.mobility++;
                                if(target==6) info.can_capture_king=true;
                                info.tactics+=PIECE_VALUES[target];
                                if(nr==0||nr==BOARD_H-1) info.tactics+=60;
                            }
                        }
                    }
                    break;
                }
                case 3: {
                    static const int dr[8]={1,1,-1,-1,2,2,-2,-2};
                    static const int dc[8]={2,-2,2,-2,1,-1,1,-1};
                    for(int d=0;d<8;d++){
                        int nr=i+dr[d],nc=j+dc[d];
                        if(nr<0||nr>=BOARD_H||nc<0||nc>=BOARD_W) continue;
                        if(self_board[nr][nc]) continue;
                        info.mobility++;
                        int target=oppn_board[nr][nc];
                        if(target){ if(target==6) info.can_capture_king=true; info.tactics+=PIECE_VALUES[target]; }
                    }
                    break;
                }
                case 6: {
                    static const int dr[8]={1,0,-1,0,1,1,-1,-1};
                    static const int dc[8]={0,1,0,-1,1,-1,1,-1};
                    for(int d=0;d<8;d++){
                        int nr=i+dr[d],nc=j+dc[d];
                        if(nr<0||nr>=BOARD_H||nc<0||nc>=BOARD_W) continue;
                        if(self_board[nr][nc]) continue;
                        info.mobility++;
                        int target=oppn_board[nr][nc];
                        if(target){ if(target==6) info.can_capture_king=true; info.tactics+=PIECE_VALUES[target]; }
                    }
                    break;
                }
                case 2: case 4: case 5: {
                    static const int dr[8]={0,0,1,-1,1,1,-1,-1};
                    static const int dc[8]={1,-1,0,0,1,-1,1,-1};
                    int st=(piece==4)?4:0, en=(piece==2)?4:8;
                    for(int d=st;d<en;d++){
                        int nr=i+dr[d],nc=j+dc[d];
                        while(nr>=0&&nr<BOARD_H&&nc>=0&&nc<BOARD_W){
                            if(self_board[nr][nc]) break;
                            info.mobility++;
                            int target=oppn_board[nr][nc];
                            if(target){ if(target==6) info.can_capture_king=true; info.tactics+=PIECE_VALUES[target]; break; }
                            nr+=dr[d]; nc+=dc[d];
                        }
                    }
                    break;
                }
            }
        }
    }
    return info;
}

/*============================================================
 * evaluate() — OPTIMIZED
 *============================================================*/
int State::evaluate(bool use_kp_eval, bool use_mobility, const GameHistory* history){
    if(this->game_state == WIN){
        return P_MAX;
    }
    if(this->step > MAX_STEP){
        int self_mat = material_total(this->board, this->player);
        int oppn_mat = material_total(this->board, 1 - this->player);
        if(self_mat > oppn_mat) return P_MAX / 2;
        if(self_mat < oppn_mat) return M_MAX / 2;
        return 0;
    }

    auto self_board = this->board.board[this->player];
    auto oppn_board = this->board.board[1 - this->player];
    int self_score = 0, oppn_score = 0;

    if(use_kp_eval){
        int self_kr=-1, self_kc=-1, oppn_kr=-1, oppn_kc=-1;
        int self_nonking=0, oppn_nonking=0;
        int self_attacker_r=-1, self_attacker_c=-1;
        int oppn_attacker_r=-1, oppn_attacker_c=-1;

        for(int i=0;i<BOARD_H;i++){
            for(int j=0;j<BOARD_W;j++){
                if(self_board[i][j]==6){ self_kr=i; self_kc=j; }
                else if(self_board[i][j]){
                    self_nonking++;
                    if(self_board[i][j]==5||self_board[i][j]==2){ self_attacker_r=i; self_attacker_c=j; }
                }
                if(oppn_board[i][j]==6){ oppn_kr=i; oppn_kc=j; }
                else if(oppn_board[i][j]){
                    oppn_nonking++;
                    if(oppn_board[i][j]==5||oppn_board[i][j]==2){ oppn_attacker_r=i; oppn_attacker_c=j; }
                }
            }
        }

        for(int i=0;i<BOARD_H;i++){
            for(int j=0;j<BOARD_W;j++){
                int sp=self_board[i][j], op=oppn_board[i][j];
                if(sp){
                    int pst_row = this->player==0 ? i : BOARD_H-1-i;
                    self_score += kp_material[sp] + pst[sp-1][pst_row][j];
                    if(sp==1) self_score += pawn_position_bonus(this->player, i, j);
                    if(oppn_kr!=-1) self_score += king_tropism(sp, i, j, oppn_kr, oppn_kc);
                }
                if(op){
                    int oppn_player = 1-this->player;
                    int pst_row = oppn_player==0 ? i : BOARD_H-1-i;
                    oppn_score += kp_material[op] + pst[op-1][pst_row][j];
                    if(op==1) oppn_score += pawn_position_bonus(oppn_player, i, j);
                    if(self_kr!=-1) oppn_score += king_tropism(op, i, j, self_kr, self_kc);
                }
            }
        }

        // King hunt bonus (endgame)
        if(oppn_nonking==0 && self_attacker_r!=-1 && oppn_kr!=-1){
            int ad = std::abs(self_attacker_r-oppn_kr)+std::abs(self_attacker_c-oppn_kc);
            int kd = std::abs(self_kr-oppn_kr)+std::abs(self_kc-oppn_kc);
            int edge = std::max(std::abs(2*oppn_kr-(BOARD_H-1)), std::abs(2*oppn_kc-(BOARD_W-1)));
            self_score += 28*(BOARD_H+BOARD_W-ad) + 12*edge + 8*(BOARD_H+BOARD_W-kd);
        }
        if(self_nonking==0 && oppn_attacker_r!=-1 && self_kr!=-1){
            int ad = std::abs(oppn_attacker_r-self_kr)+std::abs(oppn_attacker_c-self_kc);
            int kd = std::abs(oppn_kr-self_kr)+std::abs(oppn_kc-self_kc);
            int edge = std::max(std::abs(2*self_kr-(BOARD_H-1)), std::abs(2*self_kc-(BOARD_W-1)));
            oppn_score += 28*(BOARD_H+BOARD_W-ad) + 12*edge + 8*(BOARD_H+BOARD_W-kd);
        }

        self_score += piece_relation_score(this->board, this->player) / 3;
        oppn_score += piece_relation_score(this->board, 1-this->player) / 3;
        self_score += development_score(this->board, this->player);
        oppn_score += development_score(this->board, 1-this->player);

        // King safety: penalize exposed king
        if(oppn_kr!=-1){
            self_score += king_zone_pressure(this->board, this->player, oppn_kr, oppn_kc) / 4;
        }
        if(self_kr!=-1){
            oppn_score += king_zone_pressure(this->board, 1-this->player, self_kr, self_kc) / 4;
        }

    }else{
        for(int i=0;i<BOARD_H;i++){
            for(int j=0;j<BOARD_W;j++){
                self_score += simple_material[(int)self_board[i][j]];
                oppn_score += simple_material[(int)oppn_board[i][j]];
            }
        }
    }

    int bonus = 0;

    if(use_mobility){
        MobilityInfo self_info  = compute_mobility_info(this->board, this->player);
        MobilityInfo oppn_info  = compute_mobility_info(this->board, 1-this->player);

        // Balanced mobility: reward own, penalise opponent equally
        bonus += self_info.mobility - oppn_info.mobility;

        // Can capture king — big reward, but not panic-level penalty
        if(self_info.can_capture_king)  bonus += P_MAX - 2000;
        if(oppn_info.can_capture_king)  bonus -= P_MAX - 2000;

        // Tactics: own gain vs opponent threat (was 1x vs 8x — now balanced)
        bonus += (self_info.tactics - oppn_info.tactics) / 2;
    }

    bonus += max_step_material_bonus(this->board, this->player, this->step);

    (void)history;
    int material_guard = 30 * (
        material_total(this->board, this->player)
        - material_total(this->board, 1 - this->player)
    );
    return self_score - oppn_score + bonus + material_guard;
}


/*============================================================
 * Zobrist hash
 *============================================================*/
static uint64_t zobrist_piece[2][7][BOARD_H][BOARD_W];
static uint64_t zobrist_side;
static bool zobrist_ready = false;

static void init_zobrist(){
    uint64_t s = 0x7A35C9D1E4F02B68ULL;
    auto rand64 = [&s]() -> uint64_t {
        s ^= s<<13; s ^= s>>7; s ^= s<<17; return s;
    };
    for(int p=0;p<2;p++)
        for(int t=0;t<7;t++)
            for(int r=0;r<BOARD_H;r++)
                for(int c=0;c<BOARD_W;c++)
                    zobrist_piece[p][t][r][c] = rand64();
    zobrist_side = rand64();
    zobrist_ready = true;
}

uint64_t State::compute_hash_full() const{
    if(!zobrist_ready) init_zobrist();
    uint64_t h = 0;
    for(int p=0;p<2;p++)
        for(int r=0;r<BOARD_H;r++)
            for(int c=0;c<BOARD_W;c++){
                int piece = this->board.board[p][r][c];
                if(piece) h ^= zobrist_piece[p][piece][r][c];
            }
    if(this->player) h ^= zobrist_side;
    return h;
}

State* State::next_state(const Move& move){
    if(!zobrist_ready) init_zobrist();
    Board next = this->board;
    Point from = move.first, to = move.second;
    int p = this->player, opp = 1-p;
    int8_t orig_piece = next.board[p][from.first][from.second];
    int8_t moved = orig_piece;
    if(moved==1 && (to.first==BOARD_H-1||to.first==0)) moved = 5;
    uint64_t h = this->hash();
    h ^= zobrist_side;
    h ^= zobrist_piece[p][orig_piece][from.first][from.second];
    int8_t captured = next.board[opp][to.first][to.second];
    if(captured){ h ^= zobrist_piece[opp][captured][to.first][to.second]; next.board[opp][to.first][to.second]=0; }
    h ^= zobrist_piece[p][moved][to.first][to.second];
    next.board[p][from.first][from.second] = 0;
    next.board[p][to.first][to.second] = moved;
    State* ns = new State(next, opp);
    ns->step = this->step + 1;
    ns->zobrist_hash = h;
    ns->zobrist_valid = true;
    return ns;
}


static const int move_table_rook_bishop[8][7][2] = {
  {{0,1},{0,2},{0,3},{0,4},{0,5},{0,6},{0,7}},
  {{0,-1},{0,-2},{0,-3},{0,-4},{0,-5},{0,-6},{0,-7}},
  {{1,0},{2,0},{3,0},{4,0},{5,0},{6,0},{7,0}},
  {{-1,0},{-2,0},{-3,0},{-4,0},{-5,0},{-6,0},{-7,0}},
  {{1,1},{2,2},{3,3},{4,4},{5,5},{6,6},{7,7}},
  {{1,-1},{2,-2},{3,-3},{4,-4},{5,-5},{6,-6},{7,-7}},
  {{-1,1},{-2,2},{-3,3},{-4,4},{-5,5},{-6,6},{-7,7}},
  {{-1,-1},{-2,-2},{-3,-3},{-4,-4},{-5,-5},{-6,-6},{-7,-7}},
};

static const int move_table_knight[8][2] = {
  {1,2},{1,-2},{-1,2},{-1,-2},{2,1},{2,-1},{-2,1},{-2,-1},
};
static const int move_table_king[8][2] = {
  {1,0},{0,1},{-1,0},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1},
};

void State::get_legal_actions_naive(){
    this->game_state = NONE;
    std::vector<Move> all_actions;
    all_actions.reserve(64);
    auto self_board = this->board.board[this->player];
    auto oppn_board = this->board.board[1-this->player];
    int now_piece, oppn_piece;
    for(int i=0;i<BOARD_H;i++){
        for(int j=0;j<BOARD_W;j++){
            if((now_piece=self_board[i][j])){
                switch(now_piece){
                    case 1:
                        if(this->player && i<BOARD_H-1){
                            if(!oppn_board[i+1][j]&&!self_board[i+1][j]) all_actions.push_back(Move(Point(i,j),Point(i+1,j)));
                            if(j<BOARD_W-1&&(oppn_piece=oppn_board[i+1][j+1])>0){ all_actions.push_back(Move(Point(i,j),Point(i+1,j+1))); if(oppn_piece==6){this->game_state=WIN;this->legal_actions=all_actions;return;} }
                            if(j>0&&(oppn_piece=oppn_board[i+1][j-1])>0){ all_actions.push_back(Move(Point(i,j),Point(i+1,j-1))); if(oppn_piece==6){this->game_state=WIN;this->legal_actions=all_actions;return;} }
                        }else if(!this->player&&i>0){
                            if(!oppn_board[i-1][j]&&!self_board[i-1][j]) all_actions.push_back(Move(Point(i,j),Point(i-1,j)));
                            if(j<BOARD_W-1&&(oppn_piece=oppn_board[i-1][j+1])>0){ all_actions.push_back(Move(Point(i,j),Point(i-1,j+1))); if(oppn_piece==6){this->game_state=WIN;this->legal_actions=all_actions;return;} }
                            if(j>0&&(oppn_piece=oppn_board[i-1][j-1])>0){ all_actions.push_back(Move(Point(i,j),Point(i-1,j-1))); if(oppn_piece==6){this->game_state=WIN;this->legal_actions=all_actions;return;} }
                        }
                        break;
                    case 2: case 4: case 5: {
                        int st,end;
                        switch(now_piece){ case 2:st=0;end=4;break; case 4:st=4;end=8;break; case 5:st=0;end=8;break; default:st=0;end=-1; }
                        for(int part=st;part<end;part++){
                            auto move_list=move_table_rook_bishop[part];
                            for(int k=0;k<std::max(BOARD_H,BOARD_W);k++){
                                int p2[2]={move_list[k][0]+i,move_list[k][1]+j};
                                if(p2[0]>=BOARD_H||p2[0]<0||p2[1]>=BOARD_W||p2[1]<0) break;
                                now_piece=self_board[p2[0]][p2[1]];
                                if(now_piece) break;
                                all_actions.push_back(Move(Point(i,j),Point(p2[0],p2[1])));
                                oppn_piece=oppn_board[p2[0]][p2[1]];
                                if(oppn_piece){ if(oppn_piece==6){this->game_state=WIN;this->legal_actions=all_actions;return;}else break; }
                            }
                        }
                        break;
                    }
                    case 3:
                        for(auto move:move_table_knight){
                            int p2[2]={move[0]+i,move[1]+j};
                            if(p2[0]>=BOARD_H||p2[0]<0||p2[1]>=BOARD_W||p2[1]<0) continue;
                            now_piece=self_board[p2[0]][p2[1]];
                            if(now_piece) continue;
                            all_actions.push_back(Move(Point(i,j),Point(p2[0],p2[1])));
                            oppn_piece=oppn_board[p2[0]][p2[1]];
                            if(oppn_piece==6){this->game_state=WIN;this->legal_actions=all_actions;return;}
                        }
                        break;
                    case 6:
                        for(auto move:move_table_king){
                            int p2[2]={move[0]+i,move[1]+j};
                            if(p2[0]>=BOARD_H||p2[0]<0||p2[1]>=BOARD_W||p2[1]<0) continue;
                            now_piece=self_board[p2[0]][p2[1]];
                            if(now_piece) continue;
                            all_actions.push_back(Move(Point(i,j),Point(p2[0],p2[1])));
                            oppn_piece=oppn_board[p2[0]][p2[1]];
                            if(oppn_piece==6){this->game_state=WIN;this->legal_actions=all_actions;return;}
                        }
                        break;
                }
            }
        }
    }
    this->legal_actions = all_actions;
}

#define BB_SQ(r,c)  ((r)*BOARD_W+(c))
#define BB_ROW(sq)  ((sq)/BOARD_W)
#define BB_COL(sq)  ((sq)%BOARD_W)

static uint32_t bb_knight[30], bb_king[30];
static uint32_t bb_pawn_push[2][30], bb_pawn_cap[2][30];
static bool bb_ready = false;
static const int bb_dr[8]={0,0,1,-1,1,1,-1,-1};
static const int bb_dc[8]={1,-1,0,0,1,-1,1,-1};

static void bb_init(){
    static const int kn_dr[8]={1,1,-1,-1,2,2,-2,-2};
    static const int kn_dc[8]={2,-2,2,-2,1,-1,1,-1};
    static const int ki_dr[8]={1,0,-1,0,1,1,-1,-1};
    static const int ki_dc[8]={0,1,0,-1,1,-1,1,-1};
    for(int r=0;r<BOARD_H;r++){
        for(int c=0;c<BOARD_W;c++){
            int sq=BB_SQ(r,c);
            bb_knight[sq]=0;
            for(int d=0;d<8;d++){ int nr=r+kn_dr[d],nc=c+kn_dc[d]; if(nr>=0&&nr<BOARD_H&&nc>=0&&nc<BOARD_W) bb_knight[sq]|=1u<<BB_SQ(nr,nc); }
            bb_king[sq]=0;
            for(int d=0;d<8;d++){ int nr=r+ki_dr[d],nc=c+ki_dc[d]; if(nr>=0&&nr<BOARD_H&&nc>=0&&nc<BOARD_W) bb_king[sq]|=1u<<BB_SQ(nr,nc); }
            bb_pawn_push[0][sq]=0; bb_pawn_cap[0][sq]=0;
            if(r>0){ bb_pawn_push[0][sq]=1u<<BB_SQ(r-1,c); if(c>0) bb_pawn_cap[0][sq]|=1u<<BB_SQ(r-1,c-1); if(c<BOARD_W-1) bb_pawn_cap[0][sq]|=1u<<BB_SQ(r-1,c+1); }
            bb_pawn_push[1][sq]=0; bb_pawn_cap[1][sq]=0;
            if(r<BOARD_H-1){ bb_pawn_push[1][sq]=1u<<BB_SQ(r+1,c); if(c>0) bb_pawn_cap[1][sq]|=1u<<BB_SQ(r+1,c-1); if(c<BOARD_W-1) bb_pawn_cap[1][sq]|=1u<<BB_SQ(r+1,c+1); }
        }
    }
    bb_ready=true;
}

void State::get_legal_actions_bitboard(){
    if(!bb_ready) bb_init();
    this->game_state=NONE;
    this->legal_actions.clear();
    this->legal_actions.reserve(64);
    int self=this->player, oppn=1-self;
    uint32_t self_occ=0, oppn_occ=0;
    int self_pt[30]={}, oppn_pt[30]={};
    for(int r=0;r<BOARD_H;r++){
        for(int c=0;c<BOARD_W;c++){
            int sq=BB_SQ(r,c);
            if(this->board.board[self][r][c]){ self_occ|=1u<<sq; self_pt[sq]=this->board.board[self][r][c]; }
            if(this->board.board[oppn][r][c]){ oppn_occ|=1u<<sq; oppn_pt[sq]=this->board.board[oppn][r][c]; }
        }
    }
    uint32_t all_occ=self_occ|oppn_occ;
    uint32_t pieces=self_occ;
    while(pieces){
        int sq=__builtin_ctz(pieces); pieces&=pieces-1;
        int r=BB_ROW(sq),c=BB_COL(sq);
        int piece=self_pt[sq];
        uint32_t targets=0;
        switch(piece){
            case 1:{
                uint32_t push=bb_pawn_push[self][sq]&~all_occ;
                uint32_t cap=bb_pawn_cap[self][sq]&oppn_occ;
                uint32_t cs=cap;
                while(cs){ int to=__builtin_ctz(cs); cs&=cs-1; if(oppn_pt[to]==6){this->game_state=WIN;this->legal_actions.push_back(Move(Point(r,c),Point(BB_ROW(to),BB_COL(to))));return;} }
                targets=push|cap; break;
            }
            case 3:{
                targets=bb_knight[sq]&~self_occ;
                uint32_t ot=targets&oppn_occ;
                while(ot){ int to=__builtin_ctz(ot); ot&=ot-1; if(oppn_pt[to]==6){this->game_state=WIN;this->legal_actions.push_back(Move(Point(r,c),Point(BB_ROW(to),BB_COL(to))));return;} }
                break;
            }
            case 6:{
                targets=bb_king[sq]&~self_occ;
                uint32_t ot=targets&oppn_occ;
                while(ot){ int to=__builtin_ctz(ot); ot&=ot-1; if(oppn_pt[to]==6){this->game_state=WIN;this->legal_actions.push_back(Move(Point(r,c),Point(BB_ROW(to),BB_COL(to))));return;} }
                break;
            }
            case 2: case 4: case 5:{
                int ds=(piece==4)?4:0, de=(piece==2)?4:8;
                for(int d=ds;d<de;d++){
                    int cr=r+bb_dr[d],cc=c+bb_dc[d];
                    while(cr>=0&&cr<BOARD_H&&cc>=0&&cc<BOARD_W){
                        int to=BB_SQ(cr,cc); uint32_t tb=1u<<to;
                        if(self_occ&tb) break;
                        if((oppn_occ&tb)&&oppn_pt[to]==6){this->game_state=WIN;this->legal_actions.push_back(Move(Point(r,c),Point(cr,cc)));return;}
                        targets|=tb;
                        if(oppn_occ&tb) break;
                        cr+=bb_dr[d]; cc+=bb_dc[d];
                    }
                }
                break;
            }
        }
        while(targets){ int to=__builtin_ctz(targets); targets&=targets-1; this->legal_actions.push_back(Move(Point(r,c),Point(BB_ROW(to),BB_COL(to)))); }
    }
}

void State::get_legal_actions(){
#ifdef USE_BITBOARD
    get_legal_actions_bitboard();
#else
    get_legal_actions_naive();
#endif
}

const char piece_table[2][7][5] = {
  {" ","♙","♖","♘","♗","♕","♔"},
  {" ","♟","♜","♞","♝","♛","♚"}
};

std::string State::encode_output() const{
    std::stringstream ss;
    int now_piece;
    for(int i=0;i<BOARD_H;i++){
        for(int j=0;j<BOARD_W;j++){
            if((now_piece=this->board.board[0][i][j])) ss<<std::string(piece_table[0][now_piece]);
            else if((now_piece=this->board.board[1][i][j])) ss<<std::string(piece_table[1][now_piece]);
            else ss<<" ";
            ss<<" ";
        }
        ss<<"\n";
    }
    return ss.str();
}

std::string State::encode_state(){
    std::stringstream ss;
    ss<<this->player<<"\n";
    for(int pl=0;pl<2;pl++){
        for(int i=0;i<BOARD_H;i++){
            for(int j=0;j<BOARD_W;j++) ss<<int(this->board.board[pl][i][j])<<" ";
            ss<<"\n";
        }
        ss<<"\n";
    }
    return ss.str();
}

BaseState* State::create_null_state() const{
    State* s = new State(this->board, 1-this->player);
    s->get_legal_actions();
    return s;
}

static const char* piece_chars = ".PRNBQK";
static const char* piece_chars_lower = ".prnbqk";

std::string State::encode_board() const{
    std::string s;
    for(int r=0;r<BOARD_H;r++){
        if(r>0) s+='/';
        for(int c=0;c<BOARD_W;c++){
            int w=board.board[0][r][c], b=board.board[1][r][c];
            if(w>0&&w<=6) s+=piece_chars[w];
            else if(b>0&&b<=6) s+=piece_chars_lower[b];
            else s+='.';
        }
    }
    return s;
}

void State::decode_board(const std::string& s, int side_to_move){
    player=side_to_move; game_state=UNKNOWN; zobrist_valid=false; board=Board{};
    int r=0,c=0;
    for(char ch:s){
        if(ch=='/'){ r++;c=0;continue; }
        if(r>=BOARD_H||c>=BOARD_W) break;
        if(ch>='A'&&ch<='Z'){ for(int p=1;p<=6;p++) if(piece_chars[p]==ch){board.board[0][r][c]=p;break;} }
        else if(ch>='a'&&ch<='z'){ for(int p=1;p<=6;p++) if(piece_chars_lower[p]==ch){board.board[1][r][c]=p;break;} }
        c++;
    }
    get_legal_actions();
}

std::string State::cell_display(int row, int col) const{
    int w=static_cast<int>(board.board[0][row][col]);
    int b=static_cast<int>(board.board[1][row][col]);
    if(w){ const char* names=".PRNBQK"; return std::string(" ")+names[w]+" "; }
    else if(b){ const char* names=".prnbqk"; return std::string(" ")+names[b]+" "; }
    return " . ";
}

bool State::check_repetition(const GameHistory& history, int& out_score) const{
    if(history.count(hash())>=3){ out_score=-200; return true; }
    return false;
}
