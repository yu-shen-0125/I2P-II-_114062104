#include "alphabeta.hpp"
#include "pvs.hpp"

int AlphaBeta::quiescence(
    State* state,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p
){
    return alphabeta_ext::quiescence_search(
        state, alpha, beta, history, ply, ctx, p
    );
}

int AlphaBeta::eval_ab(
    State* state,
    int depth,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p
){
    return alphabeta_ext::pvs_eval(
        state, depth, alpha, beta, history, ply, ctx, p
    );
}

SearchResult AlphaBeta::search(
    State* state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    return alphabeta_ext::pvs_search(state, depth, history, ctx);
}

ParamMap AlphaBeta::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
    };
}

std::vector<ParamDef> AlphaBeta::param_defs(){
    return MiniMax::param_defs();
}
