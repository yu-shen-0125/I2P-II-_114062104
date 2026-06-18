#pragma once

#include "state.hpp"
#include "config.hpp"
#include "search_types.hpp"
#include "game_history.hpp"
#include "minimax.hpp"
#include "search_helpers.hpp"

namespace alphabeta_ext {

inline int quiescence_search(
    State* state,
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
    if(is_checkmated(state)){
        return M_MAX + ply;
    }
    if(should_stop(ctx)){
        return state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    }

    int stand_pat = state->evaluate(
        p.use_kp_eval, p.use_eval_mobility, &history
    );
    if(stand_pat >= beta){
        return beta;
    }
    if(stand_pat > alpha){
        alpha = stand_pat;
    }
    if(ply >= 12){
        return alpha;
    }

    for(const auto& action : tactical_actions(state)){
        int score;
        if(is_king_capture(state, action)){
            score = P_MAX - ply;
        }else{
            State* next = state->next_state(action);
            next->get_legal_actions();
            bool same = next->same_player_as_parent();
            int raw = same
                ? quiescence_search(next, alpha, beta, history, ply + 1, ctx, p)
                : quiescence_search(next, -beta, -alpha, history, ply + 1, ctx, p);
            score = same ? raw : -raw;
            delete next;
        }

        if(score >= beta){
            return beta;
        }
        if(score > alpha){
            alpha = score;
        }
    }

    return alpha;
}

} // namespace alphabeta_ext
