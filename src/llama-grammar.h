#pragma once

#include "llama.h"

#include <map>
#include <regex>
#include <string>
#include <vector>
#include <unordered_map>
#include <queue>
#include <set>

struct llama_vocab;

// grammar element type
enum llama_gretype {
    // end of rule definition
    LLAMA_GRETYPE_END            = 0,

    // start of alternate definition for rule
    LLAMA_GRETYPE_ALT            = 1,

    // non-terminal element: reference to rule
    LLAMA_GRETYPE_RULE_REF       = 2,

    // terminal element: character (code point)
    LLAMA_GRETYPE_CHAR           = 3,

    // inverse char(s) ([^a], [^a-b] [^abc])
    LLAMA_GRETYPE_CHAR_NOT       = 4,

    // modifies a preceding LLAMA_GRETYPE_CHAR or LLAMA_GRETYPE_CHAR_ALT to
    // be an inclusive range ([a-z])
    LLAMA_GRETYPE_CHAR_RNG_UPPER = 5,

    // modifies a preceding LLAMA_GRETYPE_CHAR or
    // LLAMA_GRETYPE_CHAR_RNG_UPPER to add an alternate char to match ([ab], [a-zA])
    LLAMA_GRETYPE_CHAR_ALT       = 6,

    // any character (.)
    LLAMA_GRETYPE_CHAR_ANY       = 7,
};

typedef struct llama_grammar_element {
    enum llama_gretype type;
    uint32_t           value; // Unicode code point or rule ID
} llama_grammar_element;

struct llama_partial_utf8 {
    uint32_t value;    // bit value so far (unshifted)
    int      n_remain; // num bytes remaining; -1 indicates invalid sequence
};

struct llama_grammar_candidate {
    size_t               index;
    const uint32_t     * code_points;
    llama_partial_utf8   partial_utf8;
};

struct token_range {
    size_t from_sorted_index;
    size_t to_sorted_index;
};


struct token_ranges {
    std::vector<token_range> allowed_token_ranges;
    std::vector<std::string> allowed_pieces;

    bool empty() const {
        return allowed_token_ranges.empty();
    }
    void fetch_pieces_for_debug(const std::vector<struct llama_grammar_token> & sorted_tokens);

    void invert(size_t size) {
        // Go from positive matches to negative matches
        // [[10, 20]] w/ size 30 -> [[0, 9], [21, 29]]
        if (allowed_token_ranges.empty()) {
            allowed_token_ranges.push_back({0, size - 1});
            return;
        }
        std::vector<token_range> new_ranges;
        if (allowed_token_ranges.front().from_sorted_index > 0) {
            new_ranges.push_back({0, allowed_token_ranges.front().from_sorted_index - 1});
        }
        for (size_t i = 1; i < allowed_token_ranges.size(); i++) {
            new_ranges.push_back({allowed_token_ranges[i - 1].to_sorted_index + 1, allowed_token_ranges[i].from_sorted_index - 1});
        }
        if (allowed_token_ranges.back().to_sorted_index < size - 1) {
            new_ranges.push_back({allowed_token_ranges.back().to_sorted_index + 1, size - 1});
        }
        allowed_token_ranges.swap(new_ranges);
    }

    void union_all(const std::vector<const token_ranges *> & ranges) {
        if (!ranges.empty()) {
            if (ranges.size() == 1) {
                *this += *ranges.front();
            } else {
                // Priority queue to merge ranges
                std::vector<token_range> merged_ranges;
                struct queue_item {
                    token_range range;
                    std::vector<token_range>::const_iterator next;
                    std::vector<token_range>::const_iterator end;

                    bool operator>(const queue_item & other) const {
                        return range.from_sorted_index > other.range.from_sorted_index;
                    }
                };
                std::priority_queue<queue_item, std::vector<queue_item>, std::greater<queue_item>> pq;
                
                // Initialize priority queue with first range from each input
                for (const auto* r : ranges) {
                    if (!r->allowed_token_ranges.empty()) {
                        pq.push({r->allowed_token_ranges.front(), 
                                r->allowed_token_ranges.begin() + 1, 
                                r->allowed_token_ranges.end()});
                    }
                }

                // Merge ranges on the fly
                while (!pq.empty()) {
                    auto top = pq.top();
                    pq.pop();

                    // Merge with previous range if possible
                    if (!merged_ranges.empty() && 
                        merged_ranges.back().to_sorted_index + 1 >= top.range.from_sorted_index) {
                        merged_ranges.back().to_sorted_index = 
                            std::max(merged_ranges.back().to_sorted_index, top.range.to_sorted_index);
                    } else {
                        merged_ranges.push_back(top.range);
                    }

                    // Add next range from the same input if available
                    if (top.next != top.end) {
                        pq.push({*top.next, top.next + 1, top.end});
                    }
                }

                allowed_token_ranges = std::move(merged_ranges);
            }

            // union all debug pieces
            std::set<std::string> pieces(allowed_pieces.begin(), allowed_pieces.end());
            for (const auto & rng : ranges) {
                for (const auto & piece : rng->allowed_pieces) {
                    pieces.insert(piece);
                }
            }
            allowed_pieces.clear();
            allowed_pieces.insert(allowed_pieces.end(), pieces.begin(), pieces.end());
        }
    }

    // Helper function to merge overlapping or adjacent ranges
    void merge_ranges() {
        if (allowed_token_ranges.empty()) {
            return;
        }
        
        std::sort(allowed_token_ranges.begin(), allowed_token_ranges.end(),
            [](const token_range& a, const token_range& b) {
                return a.from_sorted_index < b.from_sorted_index;
            });

        std::vector<token_range> merged;
        merged.push_back(allowed_token_ranges[0]);

        for (size_t i = 1; i < allowed_token_ranges.size(); i++) {
            auto& current = allowed_token_ranges[i];
            auto& last = merged.back();

            // Check if ranges overlap or are adjacent
            if (current.from_sorted_index <= last.to_sorted_index + 1) {
                // Merge the ranges
                last.to_sorted_index = std::max(last.to_sorted_index, current.to_sorted_index);
            } else {
                // Add new range
                merged.push_back(current);
            }
        }

        allowed_token_ranges.swap(merged);
    }

    token_ranges& operator+=(const token_range& other) {
        GGML_ASSERT(other.from_sorted_index <= other.to_sorted_index);
        allowed_token_ranges.push_back(other);
        merge_ranges();
        return *this;
    }

    token_ranges& operator+=(const token_ranges& other) {
        if (other.allowed_token_ranges.empty()) {
            return *this;
        }
        
        allowed_token_ranges.insert(
            allowed_token_ranges.end(),
            other.allowed_token_ranges.begin(),
            other.allowed_token_ranges.end()
        );
        
        merge_ranges();
        return *this;
    }

    token_ranges& operator+=(size_t idx) {
        return operator+=({idx, idx});
    }

    bool contains(size_t idx) const {
        // find (sorted)
        auto it = std::lower_bound(allowed_token_ranges.begin(), allowed_token_ranges.end(), idx,
            [](const token_range& range, size_t idx) {
                return range.to_sorted_index < idx;
            });
        return it != allowed_token_ranges.end() && it->from_sorted_index <= idx && idx <= it->to_sorted_index;
    }
};

using llama_grammar_rule  = std::vector<      llama_grammar_element>;
using llama_grammar_stack = std::vector<const llama_grammar_element *>;

using llama_grammar_rules      = std::vector<llama_grammar_rule>;
using llama_grammar_stacks     = std::vector<llama_grammar_stack>;
using llama_grammar_candidates = std::vector<llama_grammar_candidate>;

// TODO: remove, needed for tests atm
const llama_grammar_rules  & llama_grammar_get_rules (const struct llama_grammar * grammar);
      llama_grammar_stacks & llama_grammar_get_stacks(      struct llama_grammar * grammar);

// takes a set of possible pushdown stacks on a grammar, which are required to
// be positioned at a character range (see `llama_grammar_advance_stack`), and
// produces the N possible stacks if the given char is accepted at those
// positions
void llama_grammar_accept(struct llama_grammar * grammar, uint32_t chr);

std::vector<llama_grammar_candidate> llama_grammar_reject_candidates_for_stack(
        const llama_grammar_rules      & rules,
        const llama_grammar_stack      & stack,
        const llama_grammar_candidates & candidates);

struct llama_grammar_parser {
    std::map<std::string, uint32_t> symbol_ids;

    llama_grammar_rules rules;

    llama_grammar_stack c_rules() const;

    uint32_t get_symbol_id(const char * src, size_t len);
    uint32_t generate_symbol_id(const std::string & base_name);

    void add_rule(uint32_t rule_id, const llama_grammar_rule & rule);

    const char * parse_alternates(
            const char        * src,
            const std::string & rule_name,
            uint32_t            rule_id,
            bool                is_nested);

    const char * parse_sequence(
            const char         * src,
            const std::string  & rule_name,
            llama_grammar_rule & rule,
            bool               is_nested);

    const char * parse_rule(const char * src);

    bool parse(const char * src);
    void print(FILE * file);
};

struct llama_grammar_token {
    llama_token token;
    std::string piece;
    std::pair<std::vector<uint32_t>, llama_partial_utf8> codepoints;
};

struct llama_grammar {
    // note: allow null vocab for testing (not great)
    const llama_vocab * vocab;

    const llama_grammar_rules  rules;  // TODO: shared ptr
          llama_grammar_stacks stacks;

    std::vector<llama_grammar_token>                                sorted_tokens;
    std::vector<size_t>                                             sorted_tokens_indices; // llama_token -> idx in sorted_token
    std::unordered_map<const llama_grammar_element *, token_ranges> allowed_tokens;

    // buffer for partially generated UTF-8 sequence from accepted tokens
    llama_partial_utf8 partial_utf8;

    // lazy grammars wait for trigger words or tokens before constraining the sampling.
    // we still have trigger_tokens for non-lazy grammars to force printing of special trigger tokens.
    // (useful e.g. for tool_choice=required)
    bool                     lazy             = false;
    bool                     awaiting_trigger = false; // Initialized to true for lazy grammars only
    std::string              trigger_buffer;           // Output buffered by lazy grammar. Will be cleared once trigger is found.
    std::vector<llama_token> trigger_tokens;           // Tokens that trigger a lazy grammar, or tokens to force printing of (even if special).
    std::vector<std::pair<std::string, std::regex>>
                             trigger_patterns;         // Regular expressions that trigger a lazy grammar. Must be a full match of the entire generated
                                                       // string, and the grammar will be given the string from the first match group onwards.

};

//
// internal API
//

// note: needed for tests (not great)
struct llama_grammar * llama_grammar_init_impl(
        const struct llama_vocab * vocab,
        const llama_grammar_element ** rules,
        size_t n_rules,
        size_t start_rule_index);

struct llama_grammar * llama_grammar_init_impl(
        const struct llama_vocab * vocab,
                      const char * grammar_str,
                      const char * grammar_root,
                              bool lazy,
                     const char ** trigger_patterns,
                            size_t num_trigger_patterns,
               const llama_token * trigger_tokens,
                            size_t num_trigger_tokens);

void llama_grammar_free_impl(struct llama_grammar * grammar);

struct llama_grammar * llama_grammar_clone_impl(const struct llama_grammar & grammar);

// TODO: move the API below as member functions of llama_grammar
void llama_grammar_apply_impl(
              struct llama_grammar & grammar,
            llama_token_data_array * cur_p);

void llama_grammar_accept_impl(
              struct llama_grammar & grammar,
                       llama_token   token);

void llama_grammar_accept_str(
              struct llama_grammar & grammar,
                 const std::string & piece);
