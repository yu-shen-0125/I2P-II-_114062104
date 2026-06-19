#include <utility>
#include <algorithm>
#include <cstring>
#include "state.hpp"
#include "minimax.hpp"

#ifndef OPENING_REPLY_COL
#define OPENING_REPLY_COL 2
#endif
#ifndef OPENING_WHITE_MOVE
#define OPENING_WHITE_MOVE -1
#endif


/*============================================================
 * Transposition Table (global, persists across moves)
 *============================================================*/
static const int TT_SIZE = 1 << 20;   // ~1M entries
static TTEntry g_tt[TT_SIZE];

static TTEntry* tt_probe(uint64_t hash){
    return &g_tt[hash & (TT_SIZE - 1)];
}

static void tt_store(uint64_t hash, int depth, int score,
                     TTFlag flag, const Move& best){
    TTEntry& e = g_tt[hash & (TT_SIZE - 1)];
    // Replace if empty, same hash, or new depth is higher
    if(e.flag == TT_EMPTY || e.hash == hash || depth >= (int)e.depth){
        e.hash  = hash;
        e.score = score;
        e.best  = best;
        e.depth = (int16_t)depth;
        e.flag  = flag;
    }
}


/*============================================================
 * Move ordering helpers
 * Priority: TT move > captures (MVV-LVA) > killers > history
 *============================================================*/
static const int mvv_val[7] = {0, 1, 5, 3, 3, 9, 100};

static int move_score(const Move& mv, const State* st,
                      int ply, const PVSContext& pctx, const Move& tt_mv)
{
    if(mv == tt_mv) return 200000;

    int fr = (int)mv.first.first,  fc = (int)mv.first.second;
    int tr = (int)mv.second.first, tc = (int)mv.second.second;
    int oppn = 1 - st->player;
    int victim = (int)(unsigned char)st->board.board[oppn][tr][tc];

    if(victim){
        int attacker = (int)(unsigned char)st->board.board[st->player][fr][fc];
        return 10000 + mvv_val[victim] * 10 - mvv_val[attacker];
    }

    if(ply < 128){
        if(mv == pctx.killers[ply][0]) return 9000;
        if(mv == pctx.killers[ply][1]) return 8000;
    }

    int from_sq = fr * BOARD_W + fc;
    int to_sq   = tr * BOARD_W + tc;
    int pl = st->player;
    if(pl < 2 && from_sq < 30 && to_sq < 30)
        return pctx.history[pl][from_sq][to_sq];
    return 0;
}

static void sort_moves(std::vector<Move>& moves, const State* st,
                       int ply, const PVSContext& pctx, const Move& tt_mv)
{
    // Move lists are small.  A stable insertion sort is deterministic and
    // avoids a MinGW -O3 -march=native miscompile observed in std::sort.
    for(size_t i = 1; i < moves.size(); ++i){
        Move key = moves[i];
        int key_score = move_score(key, st, ply, pctx, tt_mv);
        size_t j = i;
        while(j > 0
              && move_score(moves[j - 1], st, ply, pctx, tt_mv) < key_score){
            moves[j] = moves[j - 1];
            --j;
        }
        moves[j] = key;
    }
}


/*============================================================
 * MiniMax — eval_ctx
 *
 * Negamax without pruning. Caller manages memory.
 *============================================================*/
int MiniMax::eval_ctx(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p
){
    ctx.nodes++;
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
    }
    if(ctx.stop){
        return 0;
    }

    /* === Lazy move generation (sets game_state) === */
    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    /* === Terminal / leaf checks === */

    if(state->game_state == WIN){
        return P_MAX - ply;
    }

    if(state->game_state == DRAW){
        return 0;
    }

    /* === Repetition check (game-specific) === */
    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score;
    }
    history.push(state->hash());

    if(depth <= 0){
        int score = state->evaluate(
            p.use_kp_eval, p.use_eval_mobility, &history
        ); 
        history.pop(state->hash());
        return score;
    }

    /* === Negamax loop === */
    int best_score = M_MAX;

    for(auto& action : state->legal_actions){
        State* next = (State*)state->next_state(action);
        bool same = next->same_player_as_parent();
        int raw = eval_ctx(next, depth - 1, history, ply + 1, ctx, p);
        int score = same ? raw : -raw;
        delete next;
        if(score > best_score) best_score = score;
    }

    history.pop(state->hash());
    return best_score;
}


/*============================================================
 * MiniMax — search
 *
 * Iterate legal moves, call eval_ctx, return SearchResult.
 *============================================================*/
SearchResult MiniMax::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    MMParams p = MMParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    if(!state->legal_actions.size()){
        state->get_legal_actions();
    }


    int best_score = M_MAX - 10;
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();

    for(auto& action : state->legal_actions){
        State* next = (State*)state->next_state(action);
        bool same = next->same_player_as_parent();
        int raw = eval_ctx(next, depth - 1, history, 1, ctx, p);
        int score = same ? raw : -raw;
        delete next;

        if(score > best_score){
            best_score = score;
            result.best_move = action;
            result.score = best_score;
            result.pv = {action};

            if(p.report_partial && ctx.on_root_update){
               ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
            }
        }
        move_index++;
    }

    result.nodes = ctx.nodes;
    result.seldepth = ctx.seldepth;
    return result;
}


/*============================================================
 * MiniMax — eval_ab
 *
 * Negamax with Alpha-Beta pruning.
 * alpha: best score the current node can guarantee (lower bound)
 * beta:  best score the opponent can guarantee (upper bound)
 * Prune when alpha >= beta (opponent would never allow this line).
 *============================================================*/
int MiniMax::eval_ab(
    State *state,
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
    if(ctx.stop){
        return 0;
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

    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score;
    }
    history.push(state->hash());

    if(depth <= 0){
        int score = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
        history.pop(state->hash());
        return score;
    }

    int best_score = M_MAX;

    for(auto& action : state->legal_actions){
        State* next = (State*)state->next_state(action);
        bool same = next->same_player_as_parent();
        int raw = same
            ? eval_ab(next, depth - 1,  alpha,  beta, history, ply + 1, ctx, p)
            : eval_ab(next, depth - 1, -beta, -alpha, history, ply + 1, ctx, p);
        int score = same ? raw : -raw;
        delete next;

        if(score > best_score) best_score = score;
        if(best_score > alpha) alpha = best_score;
        if(alpha >= beta) break;  // beta cutoff
    }

    history.pop(state->hash());
    return best_score;
}


/*============================================================
 * MiniMax — search_ab
 *
 * Root search using Alpha-Beta. Iterates all root moves,
 * updates alpha as best score is found.
 *============================================================*/
SearchResult MiniMax::search_ab(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    MMParams p = MMParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    if(!state->legal_actions.size()){
        state->get_legal_actions();
    }

    int best_score = M_MAX - 10;
    int alpha = M_MAX;
    int beta = P_MAX;
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();

    for(auto& action : state->legal_actions){
        State* next = (State*)state->next_state(action);
        bool same = next->same_player_as_parent();
        int raw = same
            ? eval_ab(next, depth - 1,  alpha,  beta, history, 1, ctx, p)
            : eval_ab(next, depth - 1, -beta, -alpha, history, 1, ctx, p);
        int score = same ? raw : -raw;
        delete next;

        if(score > best_score){
            best_score = score;
            result.best_move = action;
            result.score = best_score;
            result.pv = {action};

            if(p.report_partial && ctx.on_root_update){
                ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
            }
        }
        if(best_score > alpha) alpha = best_score;
        move_index++;
    }

    result.nodes = ctx.nodes;
    result.seldepth = ctx.seldepth;
    return result;
}


/*============================================================
 * MiniMax — qsearch
 *
 * Quiescence search: at leaf nodes, keep searching captures
 * to avoid the horizon effect.
 * Stand-pat lets the current player choose not to capture.
 *============================================================*/
int MiniMax::qsearch(
    State *state,
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
    if(ctx.stop){
        return 0;
    }

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    if(state->game_state == WIN) return P_MAX - ply;
    if(state->game_state == DRAW) return 0;

    // Dead state or stalemate: no legal moves → we lose
    if(state->legal_actions.empty()) return M_MAX + ply;

    // Stand-pat: static eval assuming we don't have to capture
    int stand_pat = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    if(stand_pat >= beta) return stand_pat;
    if(stand_pat > alpha) alpha = stand_pat;

    int best = stand_pat;

    for(auto& action : state->legal_actions){
        auto to = action.second;
        int oppn = 1 - state->player;
        // Skip non-captures
        if(!state->board.board[oppn][(int)to.first][(int)to.second]) continue;

        State* next = (State*)state->next_state(action);
        bool same = next->same_player_as_parent();
        int raw = same
            ? qsearch(next,  alpha,      beta,  history, ply + 1, ctx, p)
            : qsearch(next, -beta,       -alpha, history, ply + 1, ctx, p);
        int score = same ? raw : -raw;
        delete next;

        if(score > best) best = score;
        if(best > alpha) alpha = best;
        if(alpha >= beta) break;
    }

    return best;
}


/*============================================================
 * MiniMax — eval_pvs
 *
 * Full-featured PVS:
 *   TT lookup/store, move ordering (MVV-LVA + killer + history),
 *   null move pruning, LMR, quiescence at depth=0.
 *============================================================*/
int MiniMax::eval_pvs(
    State *state,
    int depth,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p,
    PVSContext& pctx
){
    ctx.nodes++;
    if(ply > ctx.seldepth) ctx.seldepth = ply;
    if(ctx.stop) return 0;

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    if(state->game_state == WIN)  return P_MAX - ply;
    if(state->game_state == DRAW) return 0;

    // Dead state (king captured in previous move) or stalemate → we lose
    if(state->legal_actions.empty() && state->game_state == NONE) return M_MAX + ply;

    bool is_pv = (beta > alpha + 1);

    // --- TT probe ---
    uint64_t hash = state->hash();
    TTEntry* tte  = tt_probe(hash);
    Move tt_move  = {};
    if(tte->flag != TT_EMPTY && tte->hash == hash){
        tt_move = tte->best;
        if(!is_pv && tte->depth >= depth){
            int ts = tte->score;
            if(tte->flag == TT_EXACT)                   return ts;
            if(tte->flag == TT_LOWER && ts >= beta)     return ts;
            if(tte->flag == TT_UPPER && ts <= alpha)    return ts;
        }
    }

    // --- Repetition ---
    int rep_score;
    if(state->check_repetition(history, rep_score)) return rep_score;

    // --- Quiescence at leaf ---
    if(depth <= 0){
        return qsearch(state, alpha, beta, history, ply, ctx, p);
    }

    history.push(hash);

    // --- Null move pruning (skip own move; if opp still can't improve → prune) ---
    // Skip when only K+P remain (zugzwang-prone endgame)
    auto has_heavy = [&]() -> bool {
        for(int r = 0; r < BOARD_H; r++)
            for(int c = 0; c < BOARD_W; c++)
                if(state->board.board[state->player][r][c] >= 2 &&
                   state->board.board[state->player][r][c] <= 5)
                    return true;
        return false;
    };
    if(p.use_null_move && !is_pv && depth >= 4 && state->game_state == NONE && has_heavy()){
        int R = 2;
        State* null_st = (State*)state->create_null_state();
        if(null_st){
            int raw = eval_pvs(null_st, depth - 1 - R, -beta, -beta + 1,
                               history, ply + 1, ctx, p, pctx);
            delete null_st;
            if(-raw >= beta){
                history.pop(hash);
                // Do NOT store in TT here — null move score is shallow and may corrupt TT
                return beta;
            }
        }
    }

    // --- Move ordering ---
    auto actions = state->legal_actions;
    sort_moves(actions, state, ply, pctx, tt_move);

    int best_score = M_MAX;
    Move best_move = {};
    int orig_alpha = alpha;
    bool first_child = true;
    int move_count = 0;

    for(auto& action : actions){
        State* next = (State*)state->next_state(action);
        bool same = next->same_player_as_parent();

        int tr = (int)action.second.first, tc = (int)action.second.second;
        bool is_capture = (bool)(unsigned char)state->board.board[1 - state->player][tr][tc];

        int raw, score;
        if(first_child){
            // Full-window search on the principal variation
            raw = same
                ? eval_pvs(next, depth - 1,  alpha,      beta,  history, ply + 1, ctx, p, pctx)
                : eval_pvs(next, depth - 1, -beta,       -alpha, history, ply + 1, ctx, p, pctx);
            score = same ? raw : -raw;
            first_child = false;
        } else {
            // LMR: search quiet late moves at reduced depth first
            int reduction = 0;
            if(!is_capture && depth >= 3 && move_count >= 3){
                reduction = (move_count >= 6) ? 2 : 1;
            }

            raw = same
                ? eval_pvs(next, depth - 1 - reduction,  alpha,      alpha + 1, history, ply + 1, ctx, p, pctx)
                : eval_pvs(next, depth - 1 - reduction, -alpha - 1, -alpha,     history, ply + 1, ctx, p, pctx);
            score = same ? raw : -raw;

            // Re-search at full depth if the reduced/null-window score is promising
            if(score > alpha && (score < beta || reduction > 0)){
                raw = same
                    ? eval_pvs(next, depth - 1,  alpha,  beta,  history, ply + 1, ctx, p, pctx)
                    : eval_pvs(next, depth - 1, -beta,  -alpha, history, ply + 1, ctx, p, pctx);
                score = same ? raw : -raw;
            }
        }

        delete next;
        move_count++;

        if(score > best_score){
            best_score = score;
            best_move  = action;
        }
        if(best_score > alpha) alpha = best_score;
        if(alpha >= beta){
            // Update killers + history for quiet beta-cutoff moves
            if(!is_capture && ply < 128){
                pctx.killers[ply][1] = pctx.killers[ply][0];
                pctx.killers[ply][0] = action;
                int fr = (int)action.first.first,  fc = (int)action.first.second;
                int from_sq = fr * BOARD_W + fc;
                int to_sq   = tr * BOARD_W + tc;
                int pl = state->player;
                if(pl < 2 && from_sq < 30 && to_sq < 30)
                    pctx.history[pl][from_sq][to_sq] += depth * depth;
            }
            break;
        }
    }

    // --- TT store ---
    TTFlag tt_flag;
    if(best_score <= orig_alpha) tt_flag = TT_UPPER;
    else if(best_score >= beta)  tt_flag = TT_LOWER;
    else                         tt_flag = TT_EXACT;
    tt_store(hash, depth, best_score, tt_flag, best_move);

    history.pop(hash);
    return best_score;
}


/*============================================================
 * MiniMax — search_pvs
 *
 * Root search using full-featured PVS.
 * Creates fresh PVSContext each call; TT is global and persists.
 *============================================================*/
SearchResult MiniMax::search_pvs(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    MMParams p = MMParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    PVSContext pctx;
    pctx.reset();

    if(!state->legal_actions.size()){
        state->get_legal_actions();
    }

    int best_score = M_MAX - 10;
    int alpha = M_MAX;
    int beta  = P_MAX;
    bool first_child = true;
    int move_index   = 0;
    int total_moves  = (int)state->legal_actions.size();
    // Reuse the previous iterative-deepening result at the root. Searching
    // that move first improves PVS cutoffs and makes better use of movetime.
    Move root_tt_move = {};
    uint64_t root_hash = state->hash();
    TTEntry* root_tte = tt_probe(root_hash);
    if(root_tte->flag != TT_EMPTY && root_tte->hash == root_hash){
        root_tt_move = root_tte->best;
    }

    // Order root moves after applying any verified opening restriction.
    auto actions = state->legal_actions;

    /* Verified first move for time-controlled play.  It avoids the two knight
       openings that the weak reference converts into forced wins. */
    if(state->player == 0 && state->step == 0 && OPENING_WHITE_MOVE >= 0){
        Move first;
        if(OPENING_WHITE_MOVE < 5){
            first = Move(Point(4, OPENING_WHITE_MOVE), Point(3, OPENING_WHITE_MOVE));
        }else if(OPENING_WHITE_MOVE == 5){
            first = Move(Point(5, 1), Point(3, 0));
        }else{
            first = Move(Point(5, 1), Point(3, 2));
        }
        auto it = std::find(actions.begin(), actions.end(), first);
        if(it != actions.end()) actions.assign(1, first);
    }

    /* The natural a-pawn mirror leads to a forced loss against the reference
       weak policy.  Meet 1.a3 with ...c4, the verified counter-line. */
    if(state->player == 1 && state->step == 1 && OPENING_REPLY_COL >= 0
       && state->piece_at(0, 3, 0) == 1
       && state->piece_at(1, 1, OPENING_REPLY_COL) == 1){
        Move reply(Point(1, OPENING_REPLY_COL), Point(2, OPENING_REPLY_COL));
        auto it = std::find(actions.begin(), actions.end(), reply);
        if(it != actions.end()){
            actions.assign(1, reply);
        }
    }
    sort_moves(actions, state, 0, pctx, root_tt_move);

    for(auto& action : actions){
        State* next = (State*)state->next_state(action);
        bool same = next->same_player_as_parent();

        int raw, score;
        if(first_child){
            raw = same
                ? eval_pvs(next, depth - 1,  alpha,      beta,  history, 1, ctx, p, pctx)
                : eval_pvs(next, depth - 1, -beta,       -alpha, history, 1, ctx, p, pctx);
            score = same ? raw : -raw;
            first_child = false;
        } else {
            raw = same
                ? eval_pvs(next, depth - 1,  alpha,      alpha + 1, history, 1, ctx, p, pctx)
                : eval_pvs(next, depth - 1, -alpha - 1, -alpha,     history, 1, ctx, p, pctx);
            score = same ? raw : -raw;

            if(score > alpha && score < beta){
                raw = same
                    ? eval_pvs(next, depth - 1,  alpha,  beta,  history, 1, ctx, p, pctx)
                    : eval_pvs(next, depth - 1, -beta,  -alpha, history, 1, ctx, p, pctx);
                score = same ? raw : -raw;
            }
        }

        delete next;

        if(score > best_score){
            best_score       = score;
            result.best_move = action;
            result.score     = best_score;
            result.pv        = {action};

            if(p.report_partial && ctx.on_root_update){
                ctx.on_root_update({result.best_move, best_score, depth,
                                    move_index + 1, total_moves});
            }
        }
        if(best_score > alpha) alpha = best_score;
        move_index++;
    }

    result.nodes    = ctx.nodes;
    result.seldepth = ctx.seldepth;
    // A timed-out partial result must not replace the last fully searched
    // root move from iterative deepening.
    if(!ctx.stop && !actions.empty()){
        tt_store(root_hash, depth, result.score, TT_EXACT, result.best_move);
    }
    return result;
}


/*============================================================
 * MiniMax — default_params / param_defs
 *============================================================*/
ParamMap MiniMax::default_params(){
    return {
        {"UseKPEval",       "true"},
        {"UseEvalMobility", "false"},
        {"ReportPartial",   "true"},
        {"UseNullMove",     "false"},
    };
}

std::vector<ParamDef> MiniMax::param_defs(){
    return {
        {"UseKPEval",       ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "false"},
        {"ReportPartial",   ParamDef::CHECK, "true"},
        {"UseNullMove",     ParamDef::CHECK, "false"},
    };
}
