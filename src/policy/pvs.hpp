#pragma once

#include "state.hpp"
#include "config.hpp"
#include "search_types.hpp"
#include "game_history.hpp"
#include "minimax.hpp"
#include "transposition_table.hpp"
#include "Quiescence.HPP"
#include "search_helpers.hpp"

namespace alphabeta_ext {

inline int pvs_eval(
    State* state,
    int depth,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p
){
    ctx.nodes++;
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
    }

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }
    if(state->game_state == WIN){
        return P_MAX - ply;
    }
    if(state->game_state == DRAW){
        return 0;
    }
    if(should_stop(ctx)){
        return state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    }

    uint64_t key = state->hash();
    int alpha_orig = alpha;
    int beta_orig = beta;
    int tt_score = 0;
    Move tt_best;
    bool has_tt_best = false;
    if(tt().probe(key, depth, alpha, beta, tt_score, tt_best, has_tt_best)){
        return tt_score;
    }

    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score;
    }
    history.push(key);

    if(depth <= 0){
        int score = quiescence_search(state, alpha, beta, history, ply, ctx, p);
        history.pop(key);
        return score;
    }

    int best_score = M_MAX;
    Move best_move = Move();
    bool first_child = true;

    for(const auto& action : ordered_actions(state, has_tt_best ? &tt_best : nullptr)){
        int score;
        if(is_king_capture(state, action)){
            score = P_MAX - ply;
        }else{
            State* next = state->next_state(action);
            next->get_legal_actions();
            bool same = next->same_player_as_parent();

            if(first_child){
                int raw = same
                    ? pvs_eval(next, depth - 1, alpha, beta, history, ply + 1, ctx, p)
                    : pvs_eval(next, depth - 1, -beta, -alpha, history, ply + 1, ctx, p);
                score = same ? raw : -raw;
            }else if(same){
                int raw = pvs_eval(next, depth - 1, alpha, alpha + 1, history, ply + 1, ctx, p);
                score = raw;
                if(score > alpha && score < beta){
                    score = pvs_eval(next, depth - 1, alpha, beta, history, ply + 1, ctx, p);
                }
            }else{
                int raw = pvs_eval(next, depth - 1, -alpha - 1, -alpha, history, ply + 1, ctx, p);
                score = -raw;
                if(score > alpha && score < beta){
                    raw = pvs_eval(next, depth - 1, -beta, -alpha, history, ply + 1, ctx, p);
                    score = -raw;
                }
            }

            delete next;
        }

        first_child = false;
        if(score > best_score){
            best_score = score;
            best_move = action;
        }
        if(score > alpha){
            alpha = score;
        }
        if(alpha >= beta){
            break;
        }
    }

    tt().store(key, depth, best_score, alpha_orig, beta_orig, best_move);
    history.pop(key);
    return best_score;
}

inline SearchResult pvs_search(
    State* state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    MMParams p = MMParams::from_map(ctx.params);
    SearchResult result;
    int search_depth = depth < 1 ? 1 : depth;
    result.depth = search_depth;

    if(state->legal_actions.empty()){
        state->get_legal_actions();
    }
    if(state->legal_actions.empty()){
        result.best_move = Move();
        result.score = (state->game_state == DRAW) ? 0 : M_MAX;
        result.nodes = ctx.nodes;
        result.seldepth = ctx.seldepth;
        return result;
    }

    tt().reset_if_new_root(state->hash());

    int best_score = M_MAX;
    int alpha = M_MAX;
    int beta = P_MAX;
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();
    bool have_best = false;

    Move tt_best;
    bool has_root_best = tt().root_best(state->hash(), tt_best);

    for(const auto& action : ordered_actions(state, has_root_best ? &tt_best : nullptr)){
        int score;
        if(is_king_capture(state, action)){
            score = P_MAX;
        }else{
            int player = state->player;
            int oppn = 1 - player;
            int fr = (int)action.first.first;
            int fc = (int)action.first.second;
            int tr = (int)action.second.first;
            int tc = (int)action.second.second;
            int moved = state->piece_at(player, fr, fc);
            int captured = state->piece_at(oppn, tr, tc);

            State* next = state->next_state(action);
            next->get_legal_actions();
            bool same = next->same_player_as_parent();
            bool repeats_position = history.count(next->hash()) >= 1;

            if(next->game_state == WIN){
                score = M_MAX + 1;
            }else if(!have_best){
                int raw = same
                    ? pvs_eval(next, search_depth - 1, alpha, beta, history, 1, ctx, p)
                    : pvs_eval(next, search_depth - 1, -beta, -alpha, history, 1, ctx, p);
                score = same ? raw : -raw;
            }else if(same){
                int raw = pvs_eval(next, search_depth - 1, alpha, alpha + 1, history, 1, ctx, p);
                score = raw;
                if(score > alpha && score < beta){
                    score = pvs_eval(next, search_depth - 1, alpha, beta, history, 1, ctx, p);
                }
            }else{
                int raw = pvs_eval(next, search_depth - 1, -alpha - 1, -alpha, history, 1, ctx, p);
                score = -raw;
                if(score > alpha && score < beta){
                    raw = pvs_eval(next, search_depth - 1, -beta, -alpha, history, 1, ctx, p);
                    score = -raw;
                }
            }

            score += root_hanging_penalty(next, player, action, moved, captured);
            score += immediate_promotion_threat_penalty(next, player);
            if(repeats_position){
                score -= 20000;
            }
            delete next;
        }

        score += early_root_policy_penalty(state, action);

        if(!have_best || score > best_score){
            best_score = score;
            result.best_move = action;
            have_best = true;
            if(p.report_partial && ctx.on_root_update){
                ctx.on_root_update({
                    result.best_move, best_score, search_depth,
                    move_index + 1, total_moves
                });
            }
        }
        if(score > alpha){
            alpha = score;
        }
        move_index++;
    }

    tt().store_root(state->hash(), search_depth, best_score, result.best_move);
    result.score = best_score;
    result.nodes = ctx.nodes;
    result.seldepth = ctx.seldepth;
    result.pv = {result.best_move};
    return result;
}

} // namespace alphabeta_ext
