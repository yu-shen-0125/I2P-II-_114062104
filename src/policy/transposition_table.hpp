#pragma once

#include <unordered_map>

#include "state.hpp"

namespace alphabeta_ext {

enum TTFlag {
    TT_EXACT,
    TT_LOWER,
    TT_UPPER,
};

struct TTEntry {
    int depth = -1;
    int score = 0;
    TTFlag flag = TT_EXACT;
    Move best_move = Move();
};

class TranspositionTable {
public:
    void clear(){
        table.clear();
    }

    void reset_if_new_root(uint64_t root_key){
        if(root_key != root){
            clear();
            root = root_key;
        }
    }

    bool probe(
        uint64_t key,
        int depth,
        int& alpha,
        int& beta,
        int& score,
        Move& best_move,
        bool& has_best_move
    ) const {
        auto it = table.find(key);
        if(it == table.end()){
            return false;
        }

        const TTEntry& entry = it->second;
        best_move = entry.best_move;
        has_best_move = true;

        if(entry.depth < depth){
            return false;
        }
        if(entry.flag == TT_EXACT){
            score = entry.score;
            return true;
        }

        int local_alpha = alpha;
        int local_beta = beta;
        if(entry.flag == TT_LOWER && entry.score > local_alpha){
            local_alpha = entry.score;
        }else if(entry.flag == TT_UPPER && entry.score < local_beta){
            local_beta = entry.score;
        }
        if(local_alpha >= local_beta){
            score = entry.score;
            alpha = local_alpha;
            beta = local_beta;
            return true;
        }
        return false;
    }

    void store(
        uint64_t key,
        int depth,
        int score,
        int alpha_orig,
        int beta_orig,
        const Move& best_move
    ){
        TTEntry entry;
        entry.depth = depth;
        entry.score = score;
        entry.best_move = best_move;
        if(score <= alpha_orig){
            entry.flag = TT_UPPER;
        }else if(score >= beta_orig){
            entry.flag = TT_LOWER;
        }else{
            entry.flag = TT_EXACT;
        }
        table[key] = entry;
    }

    void store_root(uint64_t key, int depth, int score, const Move& best_move){
        TTEntry entry;
        entry.depth = depth;
        entry.score = score;
        entry.flag = TT_EXACT;
        entry.best_move = best_move;
        table[key] = entry;
    }

    bool root_best(uint64_t key, Move& best_move) const {
        auto it = table.find(key);
        if(it == table.end()){
            return false;
        }
        best_move = it->second.best_move;
        return true;
    }

private:
    std::unordered_map<uint64_t, TTEntry> table;
    uint64_t root = 0;
};

inline TranspositionTable& tt(){
    static TranspositionTable table;
    return table;
}

} // namespace alphabeta_ext
