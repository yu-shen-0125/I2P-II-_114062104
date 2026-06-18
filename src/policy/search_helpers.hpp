#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <vector>

#include "state.hpp"
#include "config.hpp"
#include "search_types.hpp"

namespace alphabeta_ext {

inline int64_t now_ms(){
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

inline bool should_stop(SearchContext& ctx){
    if(ctx.stop){
        return true;
    }
    auto it = ctx.params.find("StopTimeMs");
    if(it != ctx.params.end()){
        int64_t stop_ms = std::atoll(it->second.c_str());
        if(stop_ms > 0 && now_ms() >= stop_ms){
            ctx.stop = true;
            return true;
        }
    }
    return false;
}

inline bool is_king_capture(State* state, const Move& action){
    int oppn = 1 - state->player;
    int tr = (int)action.second.first;
    int tc = (int)action.second.second;
    return state->piece_at(oppn, tr, tc) == 6;
}

inline bool is_tactical_move(State* state, const Move& action){
    int player = state->player;
    int oppn = 1 - player;
    int fr = (int)action.first.first;
    int fc = (int)action.first.second;
    int tr = (int)action.second.first;
    int tc = (int)action.second.second;
    int moved = state->piece_at(player, fr, fc);
    return state->piece_at(oppn, tr, tc) > 0
        || (moved == 1 && (tr == 0 || tr == BOARD_H - 1));
}

inline int move_order_score(State* state, const Move& action){
    int player = state->player;
    int oppn = 1 - player;
    int fr = (int)action.first.first;
    int fc = (int)action.first.second;
    int tr = (int)action.second.first;
    int tc = (int)action.second.second;
    int moved = state->piece_at(player, fr, fc);
    int captured = state->piece_at(oppn, tr, tc);
    int score = 0;

    if(captured == 6){
        score += P_MAX;
    }else if(captured){
        score += 10000 + PIECE_VALUES[captured] * 100 - PIECE_VALUES[moved] * 10;
    }

    if(moved == 1 && (tr == 0 || tr == BOARD_H - 1)){
        score += 5000;
    }else if(moved == 1 && !captured){
        score -= (tc == 0 || tc == BOARD_W - 1) ? 3500 : 1200;
    }else if((moved == 3 || moved == 4) && !captured){
        int home = player == 0 ? BOARD_H - 1 : 0;
        if(fr == home && tr != home){
            int center_dist2 = std::abs(2 * tr - (BOARD_H - 1))
                             + std::abs(2 * tc - (BOARD_W - 1));
            score += 5200 - 420 * center_dist2;
            if(tc == 0 || tc == BOARD_W - 1){
                score -= 4200;
            }
        }
    }else if(moved == 5 && !captured){
        score -= 500;
    }

    score += 16 - std::abs(2 * tr - (BOARD_H - 1))
              - std::abs(2 * tc - (BOARD_W - 1));
    return score;
}

inline std::vector<Move> ordered_actions(State* state, const Move* tt_best = nullptr){
    std::vector<Move> actions = state->legal_actions;
    std::stable_sort(actions.begin(), actions.end(),
        [state, tt_best](const Move& a, const Move& b){
            if(tt_best != nullptr){
                if(a == *tt_best && b != *tt_best){
                    return true;
                }
                if(b == *tt_best && a != *tt_best){
                    return false;
                }
            }
            return move_order_score(state, a) > move_order_score(state, b);
        }
    );
    return actions;
}

inline std::vector<Move> tactical_actions(State* state){
    std::vector<Move> actions;
    for(const auto& action : ordered_actions(state)){
        if(is_tactical_move(state, action)){
            actions.push_back(action);
        }
    }
    return actions;
}

inline bool square_occupied(State* state, int row, int col){
    return state->piece_at(0, row, col) || state->piece_at(1, row, col);
}

inline bool attacks_square(
    State* state,
    int owner,
    int from_r,
    int from_c,
    int target_r,
    int target_c
){
    int piece = state->piece_at(owner, from_r, from_c);
    if(!piece || (from_r == target_r && from_c == target_c)){
        return false;
    }

    int dr = target_r - from_r;
    int dc = target_c - from_c;
    int adr = std::abs(dr);
    int adc = std::abs(dc);

    if(piece == 1){
        int forward = owner == 0 ? -1 : 1;
        return dr == forward && adc == 1;
    }
    if(piece == 3){
        return (adr == 1 && adc == 2) || (adr == 2 && adc == 1);
    }
    if(piece == 6){
        return std::max(adr, adc) == 1;
    }

    bool rook_line = (dr == 0 || dc == 0);
    bool bishop_line = (adr == adc);
    if(piece == 2 && !rook_line){
        return false;
    }
    if(piece == 4 && !bishop_line){
        return false;
    }
    if(piece == 5 && !rook_line && !bishop_line){
        return false;
    }

    int step_r = (dr > 0) - (dr < 0);
    int step_c = (dc > 0) - (dc < 0);
    int r = from_r + step_r;
    int c = from_c + step_c;
    while(r != target_r || c != target_c){
        if(square_occupied(state, r, c)){
            return false;
        }
        r += step_r;
        c += step_c;
    }
    return true;
}

inline int count_attackers(State* state, int owner, int target_r, int target_c){
    int count = 0;
    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            if(attacks_square(state, owner, r, c, target_r, target_c)){
                count++;
            }
        }
    }
    return count;
}

inline int count_pawn_attackers(State* state, int owner, int target_r, int target_c){
    int count = 0;
    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            if(state->piece_at(owner, r, c) == 1
                && attacks_square(state, owner, r, c, target_r, target_c)){
                count++;
            }
        }
    }
    return count;
}

inline int root_hanging_penalty(State* next, int owner, const Move& action, int moved, int captured){
    int tr = (int)action.second.first;
    int tc = (int)action.second.second;
    if(moved <= 1 || moved == 6){
        return 0;
    }

    int attackers = count_attackers(next, 1 - owner, tr, tc);
    if(!attackers){
        return 0;
    }

    int defenders = count_attackers(next, owner, tr, tc);
    int pawn_attackers = count_pawn_attackers(next, 1 - owner, tr, tc);
    int base = (moved == 5) ? 76000 : (moved == 2 ? 26000 : 6500);
    if((moved == 3 || moved == 4) && pawn_attackers){
        base = 16000;
    }
    if(moved == 5 && pawn_attackers){
        base = 92000;
    }
    if(moved == 2 && pawn_attackers){
        base = 52000;
    }
    if(captured){
        base -= PIECE_VALUES[captured] * 20;
    }
    if(defenders >= attackers && !pawn_attackers){
        base /= 2;
    }
    return -std::max(0, base);
}

inline int immediate_promotion_threat_penalty(State* next, int owner){
    int side_to_move = next->player;
    if(side_to_move == owner){
        return 0;
    }

    int penalty = 0;
    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            if(next->piece_at(side_to_move, r, c) != 1){
                continue;
            }
            int dist = side_to_move == 0 ? r : (BOARD_H - 1 - r);
            if(dist <= 1){
                penalty -= 32000;
            }else if(dist == 2){
                penalty -= 9000;
            }
        }
    }
    for(const auto& reply : next->legal_actions){
        int fr = (int)reply.first.first;
        int fc = (int)reply.first.second;
        int tr = (int)reply.second.first;
        int moved = next->piece_at(side_to_move, fr, fc);
        if(moved == 1 && (tr == 0 || tr == BOARD_H - 1)){
            penalty -= 18000;
        }
    }
    return penalty;
}

inline int early_root_policy_penalty(State* state, const Move& action){
    if(state->step >= 14){
        return 0;
    }

    int player = state->player;
    int oppn = 1 - player;
    int fr = (int)action.first.first;
    int fc = (int)action.first.second;
    int tr = (int)action.second.first;
    int tc = (int)action.second.second;
    int moved = state->piece_at(player, fr, fc);
    int captured = state->piece_at(oppn, tr, tc);

    if(captured || (moved == 1 && (tr == 0 || tr == BOARD_H - 1))){
        if(captured == 1){
            int oppn_dist = oppn == 0 ? tr : (BOARD_H - 1 - tr);
            if(oppn_dist <= 1){
                return 9000;
            }
        }
        if(captured == 1){
            return 250;
        }
        if(captured == 3 || captured == 4){
            return 4200;
        }
        if(captured == 2){
            return 6500;
        }
        if(captured == 5){
            return 18000;
        }
        return 0;
    }
    if(moved == 1){
        int penalty = (tc == 0 || tc == BOARD_W - 1) ? 900 : 450;
        int advance = player == 0 ? fr - tr : tr - fr;
        if(advance > 0 && state->step < 8){
            penalty += 350;
        }
        return -penalty;
    }
    if(moved == 3 || moved == 4){
        int home = player == 0 ? BOARD_H - 1 : 0;
        if(fr == home && tr != home){
            int center_dist2 = std::abs(2 * tr - (BOARD_H - 1))
                             + std::abs(2 * tc - (BOARD_W - 1));
            int bonus = 1200 - 180 * center_dist2;
            if(tc == 0 || tc == BOARD_W - 1){
                bonus -= 1200;
            }
            return bonus;
        }
    }
    if(moved == 2){
        int home = player == 0 ? BOARD_H - 1 : 0;
        if(state->step < 18){
            if(!captured){
                return -5200;
            }
        }
        if(fr == home && tr != home){
            return 80;
        }
    }
    if(moved == 5){
        return -80;
    }
    return 0;
}

inline bool is_checkmated(State*){
    return false;
}

} // namespace alphabeta_ext
