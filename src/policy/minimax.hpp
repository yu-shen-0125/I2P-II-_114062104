#pragma once
#include "search_types.hpp"
#include "game_history.hpp"
#include <cstring>
#include <cstdint>

// Transposition table flags
enum TTFlag : uint8_t { TT_EXACT = 0, TT_LOWER = 1, TT_UPPER = 2, TT_EMPTY = 3 };

// Transposition table entry
struct TTEntry {
    uint64_t hash  = 0;
    int      score = 0;
    Move     best  = {};
    int16_t  depth = -1;
    TTFlag   flag  = TT_EMPTY;
};

// Per-search context: killer + history tables for move ordering
struct PVSContext {
    Move killers[128][2];
    int  history[2][30][30];   // [player][from_sq][to_sq],  sq = row*5 + col

    void reset(){
        for(auto& row : killers){ row[0] = Move{}; row[1] = Move{}; }
        memset(history, 0, sizeof(history));
    }
};

struct MMParams {
    bool use_kp_eval       = true;
    bool use_eval_mobility = true;
    bool report_partial    = true;
    bool use_null_move     = true;

    static MMParams from_map(const ParamMap& m){
        MMParams p;
        p.use_kp_eval       = param_bool(m, "UseKPEval",       true);
        p.use_eval_mobility = param_bool(m, "UseEvalMobility", true);
        p.report_partial    = param_bool(m, "ReportPartial",   true);
        p.use_null_move     = param_bool(m, "UseNullMove",     true);
        return p;
    }
};

class MiniMax{
public:
    static int eval_ctx(
        State *state,
        int depth,
        GameHistory& history,
        int ply,
        SearchContext& ctx,
        const MMParams& p
    );
    static SearchResult search(
        State *state,
        int depth,
        GameHistory& history,
        SearchContext& ctx
    );

    static int eval_ab(
        State *state,
        int depth,
        int alpha,
        int beta,
        GameHistory& history,
        int ply,
        SearchContext& ctx,
        const MMParams& p
    );
    static SearchResult search_ab(
        State *state,
        int depth,
        GameHistory& history,
        SearchContext& ctx
    );

    static int qsearch(
        State *state,
        int alpha,
        int beta,
        GameHistory& history,
        int ply,
        SearchContext& ctx,
        const MMParams& p
    );

    // Full-featured PVS: TT + move ordering + null move + LMR
    static int eval_pvs(
        State *state,
        int depth,
        int alpha,
        int beta,
        GameHistory& history,
        int ply,
        SearchContext& ctx,
        const MMParams& p,
        PVSContext& pctx
    );
    static SearchResult search_pvs(
        State *state,
        int depth,
        GameHistory& history,
        SearchContext& ctx
    );

    static ParamMap default_params();
    static std::vector<ParamDef> param_defs();
};
